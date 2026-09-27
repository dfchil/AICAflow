#include "host_internal.h"

void maps_reset(void) {
    memset(g_map_used, 0, sizeof(g_map_used));
    uint32_t invalid[AFX_CHANNEL_MAP_ENTRIES];
    for (uint32_t i = 0; i < AFX_CHANNEL_MAP_ENTRIES; ++i) invalid[i] = UINT32_MAX;
    for (uint32_t arena = 0; arena < AFX_CHANNEL_MAP_ARENAS; ++arena)
        upload_words(AFX_CHANNEL_MAP_ARENA_ADDR + arena * AFX_CHANNEL_MAP_ARENA_SIZE,
                     invalid, sizeof(invalid));
}
static uint64_t allocate_channels(uint8_t count) {
    if (!count || count > 64) return 0;
    uint64_t picked = 0;
    for (uint32_t bit = 0; bit < 64 && count; ++bit)
        if (g_available_channels & (UINT64_C(1) << bit)) {
            picked |= UINT64_C(1) << bit;
            --count;
        }
    if (count) return 0;
    g_available_channels &= ~picked;
    return picked;
}
static bool map_allocate(uint8_t count, uint64_t channels, uint32_t *address,
                         uint8_t *arena_out, uint8_t *offset_out) {
    uint8_t arena = count <= 4 ? 0 : count <= 8 ? 1 : count <= 16 ? 2 : count <= 32 ? 3 : 4;
    uint64_t needed = count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1u;
    for (uint32_t offset = 0; offset + count <= AFX_CHANNEL_MAP_ENTRIES; ++offset) {
        uint64_t placed = needed << offset;
        if (g_map_used[arena] & placed) continue;
        g_map_used[arena] |= placed;
        uint32_t base = AFX_CHANNEL_MAP_ARENA_ADDR + arena * AFX_CHANNEL_MAP_ARENA_SIZE +
                        offset * AFX_CHANNEL_MAP_ENTRY_BYTES;
        uint32_t entries[AFX_CHANNEL_MAP_ENTRIES];
        uint32_t local = 0;
        for (uint32_t physical = 0; physical < 64; ++physical)
            if (channels & (UINT64_C(1) << physical)) {
                entries[local++] = physical;
            }
        upload_words(base, entries, count * sizeof(*entries));
        *address = base;
        *arena_out = arena;
        *offset_out = offset;
        return true;
    }
    return false;
}
static void map_release(const afx_instance_slot_t *slot) {
    if (!slot->required_channels) return;
    uint64_t mask = slot->required_channels == 64 ? UINT64_MAX :
                    (UINT64_C(1) << slot->required_channels) - 1u;
    g_map_used[slot->map_arena] &= ~(mask << slot->map_offset);
    uint32_t invalid[AFX_CHANNEL_MAP_ENTRIES];
    for (uint32_t i = 0; i < slot->required_channels; ++i) invalid[i] = UINT32_MAX;
    upload_words(slot->map_addr, invalid, slot->required_channels * sizeof(*invalid));
}
bool resolve_instance(afx_instance_t instance, uint32_t *index) {
    if (!instance || !index) return false;
    uint32_t raw = instance & 0xffffu;
    if (!raw || raw > AFX_MAX_FLOW_SLOTS) return false;
    afx_instance_slot_t *slot = &g_instances[raw - 1u];
    if (!slot->live || slot->generation != AFX_HANDLE_GENERATION(instance)) return false;
    *index = raw - 1u;
    return true;
}
void release_instance_work(afx_instance_slot_t *slot) {
    if (!slot->work_reserved) return;
    const afx_asset_slot_t *flow = &g_assets[slot->asset_index];
    g_reserved_peak_commands -= flow->peak_commands;
    g_reserved_peak_writes -= flow->peak_register_writes;
    slot->work_reserved = false;
}
void release_instance_preserving_generation(uint32_t index) {
    afx_instance_slot_t *slot = &g_instances[index];
    uint16_t generation = slot->generation;
    bool retired = slot->retired;
    if (slot->staging_addr) (void)free_allocation(slot->staging_addr);
    g_available_channels |= slot->channel_mask;
    map_release(slot);
    if (slot->asset_index < g_asset_capacity && g_assets[slot->asset_index].references)
        --g_assets[slot->asset_index].references;
    release_instance_work(slot);
    if (generation == UINT16_MAX) retired = true;
    else ++generation;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
    slot->retired = retired;
}

int afx_instance_activate(afx_asset_t asset, afx_instance_t *out) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t asset_index;
    if (!out || !resolve_asset(asset, &asset_index) || !g_assets[asset_index].flow)
        return -AFX_INVALID_HANDLE;
    afx_asset_slot_t *flow = &g_assets[asset_index];
    uint8_t count = (uint8_t)flow->header.required_channels;
    if (flow->peak_commands > AFX_EXECUTION_BUDGET_COMMANDS - g_reserved_peak_commands ||
        flow->peak_register_writes > AFX_EXECUTION_BUDGET_WRITES - g_reserved_peak_writes)
        return -AFX_NO_EXEC_BUDGET;
    uint32_t index = 0;
    while (index < AFX_MAX_FLOW_SLOTS && (g_instances[index].live || g_instances[index].retired)) ++index;
    if (index == AFX_MAX_FLOW_SLOTS) return -AFX_NO_FLOW_SLOTS;
    uint64_t channels = allocate_channels(count);
    if (!channels) return -AFX_NO_CHANNELS;
    uint32_t map_address; uint8_t arena, offset;
    if (!map_allocate(count, channels, &map_address, &arena, &offset)) {
        g_available_channels |= channels;
        return -AFX_NO_FLOW_SLOTS;
    }
    afx_instance_slot_t *slot = &g_instances[index];
    if (!slot->generation) slot->generation = 1;
    afx_instance_t reference = AFX_MAKE_HANDLE(index, slot->generation);
    /* ARM deadlines are absolute. A one-tick lead admits queue/G2 latency while
     * preserving the authored deltas instead of making later flows catch up. */
    uint32_t start_tick = read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, timer_ticks)) + 1u;
    afx_activation_t activation = {
        .image_base = flow->addr, .image_size = flow->size,
        .stream_offset = flow->header.stream_offset, .stream_size = flow->header.stream_size,
        .setups_offset = flow->header.setups_offset, .setup_count = flow->header.setup_count,
        .channel_map = map_address, .required_channels = count,
        .flags = flow->header.flags & (AFX_FLAG_CONTROLLED | AFX_FLAG_MUSIC |
                                       AFX_FLAG_MUSIC_CHORUS | AFX_FLAG_LANES),
        .start_tick = start_tick
    };
    uint32_t sequence = new_sequence();
    int queued = enqueue(AFX_CMD_ACTIVATE, reference, sequence, 0, &activation, sizeof(activation));
    if (queued) {
        afx_instance_slot_t rollback = { .required_channels = count, .map_addr = map_address,
            .map_arena = arena, .map_offset = offset };
        map_release(&rollback);
        g_available_channels |= channels;
        return queued;
    }
    slot->live = true;
    slot->pending = true;
    slot->asset_index = asset_index;
    slot->map_addr = map_address;
    slot->required_channels = count;
    slot->map_arena = arena;
    slot->map_offset = offset;
    slot->channel_mask = channels;
    slot->start_tick = start_tick;
    slot->status = (afx_instance_status_t){ .reference = reference, .state = AFX_FREE,
                                             .sequence = sequence, .result = AFX_OK };
    ++flow->references;
    g_reserved_peak_commands += flow->peak_commands;
    g_reserved_peak_writes += flow->peak_register_writes;
    slot->work_reserved = true;
    *out = reference;
    return AFX_OK;
}
static int lifecycle(afx_instance_t instance, uint32_t opcode) {
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    afx_instance_slot_t *slot = &g_instances[index];
    if (slot->pending) return -AFX_BUSY;
    uint32_t sequence = new_sequence();
    int queued = enqueue(opcode, instance, sequence, 0, NULL, 0);
    if (!queued) {
        slot->pending = true;
        slot->recycling = opcode == AFX_CMD_RECYCLE;
        slot->status.sequence = sequence;
    }
    return queued;
}
int afx_instance_stop(afx_instance_t instance) {
    HOST_GUARD(-AFX_BUSY); return lifecycle(instance, AFX_CMD_STOP); }
int afx_instance_pause(afx_instance_t instance) {
    HOST_GUARD(-AFX_BUSY); return lifecycle(instance, AFX_CMD_PAUSE); }
int afx_instance_rebuild(afx_instance_t instance, const afx_restore_channel_t *states,
                         uint32_t state_count, uint32_t stream_position,
                         uint32_t local_tick, uint32_t next_deadline, bool run) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    afx_instance_slot_t *slot = &g_instances[index];
    if (slot->pending || slot->status.state != AFX_PAUSED || state_count > slot->required_channels ||
        (state_count && !states)) return -AFX_BAD_COMMAND;
    afx_asset_slot_t *flow = &g_assets[slot->asset_index];
    if (flow->sfx) return -AFX_UNSUPPORTED; /* Use bound NOTE setups, not raw restore addresses. */
    if (stream_position < flow->header.stream_offset ||
        stream_position >= flow->header.stream_offset + flow->header.stream_size) return -AFX_BAD_BOUNDS;
    uint32_t seen_low = 0, seen_high = 0;
    for (uint32_t i = 0; i < state_count; ++i) {
        uint32_t local = states[i].local_channel;
        uint32_t bit = 1u << (local & 31u);
        uint32_t *seen = local < 32 ? &seen_low : &seen_high;
        if (local >= slot->required_channels || (*seen & bit)) return -AFX_BAD_COMMAND;
        *seen |= bit;
    }
    uint32_t staging = 0;
    if (state_count) {
        staging = afx_mem_alloc(state_count * sizeof(*states), AFX_UPLOAD_ALIGN);
        if (!staging) return -AFX_NO_AICA_RAM;
        if (afx_mem_upload(staging, states, state_count * sizeof(*states))) {
            (void)afx_mem_free(staging);
            return -AFX_BAD_BOUNDS;
        }
    }
    afx_rebuild_payload_t request = { .states_address = staging, .state_count = state_count,
        .stream_position = stream_position, .local_tick = local_tick, .next_deadline = next_deadline };
    uint32_t sequence = new_sequence();
    int queued = enqueue(AFX_CMD_REBUILD, instance, sequence,
                         run ? AFX_REBUILD_RUN : 0, &request, sizeof(request));
    if (queued) {
        if (staging) (void)afx_mem_free(staging);
        return queued;
    }
    slot->pending = true;
    slot->staging_addr = staging;
    slot->status.sequence = sequence;
    return AFX_OK;
}
static int checkpoint_states(const afx_asset_slot_t *flow, uint32_t tick,
                             afx_restore_channel_t **states_out, uint32_t *count_out,
                             uint32_t *checkpoint_tick_out, uint32_t *position_out,
                             uint32_t *remaining_out) {
    const uint8_t *data = flow->checkpoints;
    uint32_t size = flow->checkpoints_size;
    if (!data || size < 16) return -AFX_UNSUPPORTED;
    if (afx_read32(data) != AFX_CHECKPOINT_MAGIC) return -AFX_UNSUPPORTED;
    if (afx_read32(data + 4) != AFX_CHECKPOINT_VERSION || afx_read32(data + 12)) return -AFX_BAD_FORMAT;
    uint32_t count = afx_read32(data + 8), cursor = 16, selected = UINT32_MAX, previous = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (cursor > size || size - cursor < 16) return -AFX_BAD_FORMAT;
        uint32_t at = afx_read32(data + cursor), states = afx_read32(data + cursor + 12);
        if ((i && at < previous) || states > flow->header.required_channels ||
            states > (size - cursor - 16) / sizeof(afx_restore_channel_t)) return -AFX_BAD_FORMAT;
        if (at <= tick) selected = cursor;
        previous = at;
        cursor += 16 + states * sizeof(afx_restore_channel_t);
    }
    if (cursor != size || selected == UINT32_MAX) return -AFX_BAD_FORMAT;
    uint32_t position = afx_read32(data + selected + 4);
    uint32_t remaining = afx_read32(data + selected + 8);
    uint32_t states = afx_read32(data + selected + 12);
    if (position < flow->header.stream_offset ||
        position >= flow->header.stream_offset + flow->header.stream_size) return -AFX_BAD_FORMAT;
    afx_restore_channel_t *out = states ? malloc(states * sizeof(*out)) : NULL;
    if (states && !out) return -AFX_NO_HOST_RAM;
    uint32_t seen_low = 0, seen_high = 0, source = selected + 16;
    for (uint32_t i = 0; i < states; ++i, source += sizeof(*out)) {
        uint32_t local = afx_read32(data + source);
        uint32_t bit = 1u << (local & 31u);
        uint32_t *seen = local < 32 ? &seen_low : &seen_high;
        if (local >= flow->header.required_channels || (*seen & bit)) { free(out); return -AFX_BAD_FORMAT; }
        *seen |= bit;
        out[i].local_channel = local;
        for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field)
            out[i].fields[field] = afx_read16(data + source + 4 + field * 2);
        if (!(out[i].fields[AFX_FIELD_CONTROL] & 0x400u)) {
            uint32_t relative = ((out[i].fields[AFX_FIELD_CONTROL] & 0x7fu) << 16) |
                                out[i].fields[AFX_FIELD_SAMPLE_LOW];
            if (relative >= flow->size || flow->addr > AFX_ASSET_LIMIT - relative) {
                free(out); return -AFX_BAD_FORMAT;
            }
            uint32_t address = flow->addr + relative;
            out[i].fields[AFX_FIELD_CONTROL] = (out[i].fields[AFX_FIELD_CONTROL] & ~0x7fu) |
                                                   ((address >> 16) & 0x7fu);
            out[i].fields[AFX_FIELD_SAMPLE_LOW] = address;
        }
    }
    *states_out = out; *count_out = states; *checkpoint_tick_out = afx_read32(data + selected);
    *position_out = position; *remaining_out = remaining;
    return AFX_OK;
}
static int flow_read(const afx_asset_slot_t *flow, uint32_t offset, void *data, uint32_t size) {
    if (offset > flow->size || size > flow->size - offset) return -AFX_BAD_BOUNDS;
    if (flow->image) { memcpy(data, flow->image + offset, size); return AFX_OK; }
    uint8_t *out = data;
    while (size) {
        uint32_t address = flow->addr + offset, skip = address & 3u;
        uint32_t word = g2_read_32(g_spu_base + (address & ~3u));
        uint32_t count = size < 4u - skip ? size : 4u - skip;
        for (uint32_t i = 0; i < count; ++i) out[i] = (uint8_t)(word >> (8u * (skip + i)));
        out += count;
        offset += count;
        size -= count;
    }
    return AFX_OK;
}
static int replay_checkpoint(const afx_asset_slot_t *flow, uint32_t target,
                             uint32_t checkpoint_tick, uint32_t position, uint32_t remaining,
                             afx_restore_channel_t **states_out, uint32_t *count_out,
                             uint32_t *position_out, uint32_t *remaining_out) {
    if (position >= flow->size || target < checkpoint_tick) return -AFX_BAD_FORMAT;
    afx_restore_channel_t active[AFX_MAX_FLOW_CHANNELS];
    uint8_t present[AFX_MAX_FLOW_CHANNELS] = {0};
    uint32_t count = *count_out;
    if (count > flow->header.required_channels) return -AFX_BAD_FORMAT;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t local = (*states_out)[i].local_channel;
        if (local >= flow->header.required_channels || present[local]) return -AFX_BAD_FORMAT;
        active[local] = (*states_out)[i]; present[local] = 1;
    }
    if (remaining > UINT32_MAX - checkpoint_tick) return -AFX_BAD_BOUNDS;
    uint32_t cursor = position, when = checkpoint_tick + remaining;
    while (cursor < flow->header.stream_offset + flow->header.stream_size) {
        uint8_t event_data[8 + AFX_SETUP_BYTES];
        uint32_t available = flow->header.stream_offset + flow->header.stream_size - cursor;
        if (available > sizeof(event_data)) available = sizeof(event_data);
        int read = flow_read(flow, cursor, event_data, available);
        if (read) return read;
        afx_event_t event;
        afx_result_t result = afx_decode_event(event_data, available, &event);
        if (result) return -(int)result;
        cursor += event.bytes;
        if (event.opcode == AFX_OP_WAIT8 || event.opcode == AFX_OP_WAIT16 || event.opcode == AFX_OP_WAIT32) {
            if (event.wait > UINT32_MAX - when) return -AFX_BAD_BOUNDS;
            when += event.wait;
            continue;
        }
        if (when > target) { cursor -= event.bytes; break; }
        if (event.opcode == AFX_OP_NOTE) {
            uint8_t setup[AFX_SETUP_BYTES];
            if (event.channel >= flow->header.required_channels || event.setup >= flow->header.setup_count ||
                flow_read(flow, flow->header.setups_offset + event.setup * AFX_SETUP_BYTES,
                          setup, sizeof(setup)) ||
                afx_apply_setup_fields(active[event.channel].fields, setup, event.mask, event.values,
                                       afx_field_value_bytes(event.mask))) return -AFX_BAD_FORMAT;
            active[event.channel].local_channel = event.channel;
            present[event.channel] = 1;
        } else if (event.opcode == AFX_OP_KEYOFF) {
            if (event.channel >= flow->header.required_channels) return -AFX_BAD_FORMAT;
            present[event.channel] = 0;
        } else if (event.opcode == AFX_OP_PATCH) {
            if (event.channel >= flow->header.required_channels) return -AFX_BAD_FORMAT;
            /* KEYOFF voices are omitted by checkpoint_plan. Updates to their
             * release tails remain legal, but must not resurrect a voice. */
            if (present[event.channel] &&
                afx_apply_fields(active[event.channel].fields, NULL, event.mask, event.values,
                    afx_field_value_bytes(event.mask))) return -AFX_BAD_FORMAT;
        } else if (event.opcode == AFX_OP_END || event.opcode == AFX_OP_PARK) {
            return -AFX_BAD_BOUNDS;
        } else return -AFX_BAD_FORMAT;
    }
    if (cursor >= flow->header.stream_offset + flow->header.stream_size || when <= target)
        return -AFX_BAD_BOUNDS;
    afx_restore_channel_t *out = NULL;
    uint32_t rebuilt = 0;
    for (uint32_t local = 0; local < flow->header.required_channels; ++local)
        if (present[local]) ++rebuilt;
    if (rebuilt) {
        out = malloc(rebuilt * sizeof(*out));
        if (!out) return -AFX_NO_HOST_RAM;
        uint32_t index = 0;
        for (uint32_t local = 0; local < flow->header.required_channels; ++local)
            if (present[local]) out[index++] = active[local];
    }
    free(*states_out);
    *states_out = out; *count_out = rebuilt; *position_out = cursor;
    *remaining_out = when - target;
    return AFX_OK;
}
int afx_instance_seek(afx_instance_t instance, uint32_t tick) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    afx_instance_slot_t *slot = &g_instances[index];
    if (slot->pending || slot->status.state != AFX_PAUSED) return -AFX_BUSY;
    afx_restore_channel_t *states;
    uint32_t checkpoint_tick, count, position, remaining;
    int result = checkpoint_states(&g_assets[slot->asset_index], tick, &states, &count,
                                   &checkpoint_tick, &position, &remaining);
    if (result) return result;
    result = replay_checkpoint(&g_assets[slot->asset_index], tick, checkpoint_tick, position,
                               remaining, &states, &count, &position, &remaining);
    if (result) { free(states); return result; }
    result = afx_instance_rebuild(instance, states, count, position, remaining,
                                  afx_status_timer_ticks() + 1u, true);
    free(states);
    return result;
}
int afx_instance_patch(afx_instance_t instance, uint8_t local_channel,
                       uint32_t mask, const uint16_t *values) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    if (g_instances[index].pending && g_instances[index].status.state != AFX_FREE)
        return -AFX_BUSY;
    if (local_channel >= g_instances[index].required_channels || (mask & ~AFX_FIELD_MASK))
        return -AFX_BAD_COMMAND;
    if (g_assets[g_instances[index].asset_index].sfx && (mask & 0x0fu))
        return -AFX_BAD_COMMAND; /* Sample bindings and bounds cannot change via IPC. */
    uint32_t count = afx_field_value_bytes(mask) / 2u;
    if (count && !values) return -AFX_BAD_BOUNDS;
    afx_patch_payload_t patch = { .local_channel = local_channel, .mask = mask };
    for (uint32_t i = 0; i < count; ++i) patch.values[i] = values[i];
    return enqueue(AFX_CMD_PATCH, instance, new_sequence(), 0, &patch, sizeof(patch));
}
int afx_instance_gain(afx_instance_t instance, uint8_t gain) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    if (g_instances[index].pending && g_instances[index].status.state != AFX_FREE)
        return -AFX_BUSY;
    afx_gain_payload_t request = { .gain = gain };
    return enqueue(AFX_CMD_INSTANCE_GAIN, instance, new_sequence(), 0, &request, sizeof(request));
}
int afx_instance_tempo(afx_instance_t instance, uint16_t scale_q8_8) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    if (g_instances[index].pending && g_instances[index].status.state != AFX_FREE)
        return -AFX_BUSY;
    if (scale_q8_8 < 16u || scale_q8_8 > 4096u) return -AFX_BAD_COMMAND;
    afx_tempo_payload_t request = {
        .period_q8_8 = (65536u + scale_q8_8 / 2u) / scale_q8_8
    };
    return enqueue(AFX_CMD_INSTANCE_TEMPO, instance, new_sequence(), 0,
                   &request, sizeof(request));
}
int afx_instance_lanes_set(afx_instance_t instance, uint32_t modifier,
                           uint8_t first_lane, uint32_t mask, const uint8_t values[32]) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    const afx_instance_slot_t *slot = &g_instances[index];
    const afx_asset_slot_t *flow = &g_assets[slot->asset_index];
    if (slot->pending && slot->status.state != AFX_FREE) return -AFX_BUSY;
    if (!(flow->header.flags & AFX_FLAG_LANES) || modifier >= AFX_LANE_MODIFIER_COUNT ||
        first_lane > AFX_MAX_FLOW_CHANNELS - 32u || !mask || !values) return -AFX_BAD_COMMAND;
    afx_lane_payload_t request = { .first_lane = first_lane, .mask = mask };
    memcpy(request.values, values, sizeof(request.values));
    return enqueue(AFX_CMD_LANE_SET, instance, new_sequence(), modifier, &request, sizeof(request));
}
int afx_instance_recycle(afx_instance_t instance) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    if (g_instances[index].pending || (g_instances[index].status.state != AFX_DONE &&
                                       g_instances[index].status.state != AFX_ERROR))
        return -AFX_BUSY;
    return lifecycle(instance, AFX_CMD_RECYCLE);
}
int afx_update(void) {
    HOST_GUARD(-AFX_BUSY);
    if (!g_ready || !g_lifecycle) return -AFX_UNSUPPORTED;
    for (uint32_t index = 0; index < AFX_MAX_FLOW_SLOTS; ++index) {
        afx_instance_slot_t *slot = &g_instances[index];
        if (!slot->live) continue;
        afx_instance_status_t observed;
        if (!read_observed(index, &observed)) continue;
        afx_instance_t reference = AFX_MAKE_HANDLE(index, slot->generation);
        /* FREE carries no generation in its reference, so its sequence is the
         * guard against a delayed recycle observation freeing this slot after
         * it has been reused by a later generation. */
        if (slot->recycling && observed.reference == 0 && observed.state == AFX_FREE &&
            observed.sequence == slot->status.sequence) {
            release_instance_preserving_generation(index);
            continue;
        }
        if (observed.reference != reference || !observed.sequence ||
            observed.sequence < slot->status.sequence) continue;
        /* Bound SFX cannot rebuild; PARK permanently ends their stream work.
         * Keep channel/sample ownership and the independent IPC limits intact. */
        if (observed.state == AFX_PARKED && g_assets[slot->asset_index].sfx)
            release_instance_work(slot);
        uint32_t expected = slot->status.sequence;
        if (memcmp(&observed, &slot->status, sizeof(observed)))
            slot->status = observed;
        if (slot->pending && observed.sequence >= expected)
            {
                if (slot->staging_addr) {
                    (void)free_allocation(slot->staging_addr);
                    slot->staging_addr = 0;
                }
                slot->pending = false;
            }
    }
    return AFX_OK;
}
int afx_instance_status(afx_instance_t instance, afx_instance_status_t *out) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!out || !resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    *out = g_instances[index].status;
    return AFX_OK;
}
uint32_t afx_instance_start_tick(afx_instance_t instance) {
    HOST_GUARD(0);
    uint32_t index;
    return resolve_instance(instance, &index) ? g_instances[index].start_tick : 0;
}
