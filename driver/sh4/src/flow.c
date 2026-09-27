#include "host_internal.h"

static afx_result_t profile_stream(const afx_file_header_t *header, const uint8_t *image,
                                   uint32_t *peak_commands, uint32_t *peak_writes) {
    uint32_t cursor = header->stream_offset, end = cursor + header->stream_size;
    uint32_t commands = 0, writes = 0, max_commands = 0, max_writes = 0;
    while (cursor < end) {
        afx_event_t event;
        afx_result_t result = afx_decode_event(image + cursor, end - cursor, &event);
        if (result) return result;
        cursor += event.bytes;
        if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) {
            if (commands > max_commands) max_commands = commands;
            if (writes > max_writes) max_writes = writes;
            commands = writes = 0;
        } else if (event.opcode == AFX_OP_NOTE) {
            ++commands; writes += 19;
        } else if (event.opcode == AFX_OP_PATCH) {
            ++commands; writes += afx_field_value_bytes(event.mask) / 2u;
        } else if (event.opcode == AFX_OP_KEYOFF) {
            ++commands; ++writes;
        }
    }
    if (commands > max_commands) max_commands = commands;
    if (writes > max_writes) max_writes = writes;
    *peak_commands = max_commands; *peak_writes = max_writes;
    return AFX_OK;
}

int afx_flow_upload_begin(const void *data, uint32_t size, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    afx_file_header_t header;
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    afx_result_t valid = afx_file_validate(data, size, &header);
    if (valid) return -(int)valid;
    uint8_t *checkpoints = NULL;
    if (header.checkpoints_size) {
        checkpoints = malloc(header.checkpoints_size);
        if (!checkpoints) return -AFX_NO_HOST_RAM;
        memcpy(checkpoints, (const uint8_t *)data + header.checkpoints_offset,
               header.checkpoints_size);
    }
    uint8_t *linked = upload_image_alloc(header.image_size);
    if (!linked) { free(checkpoints); return -AFX_NO_HOST_RAM; }
    afx_mem_diagnostic_t diagnostic;
    if (!allocation_diagnostic(header.image_size, AFX_UPLOAD_ALIGN, &diagnostic)) {
        free(linked); free(checkpoints); return -AFX_BAD_BOUNDS;
    }
    if (diagnostic.result != AFX_MEM_AVAILABLE) {
        free(linked); free(checkpoints); return -AFX_NO_AICA_RAM;
    }
    afx_asset_t asset = reserve_asset(header.image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) { free(linked); free(checkpoints); return -AFX_NO_AICA_RAM; }
    valid = afx_link_validated_image(data, &header, afx_asset_addr(asset), linked, header.image_size);
    uint32_t peak_commands = 0, peak_writes = 0;
    if (!valid) valid = profile_stream(&header, linked, &peak_commands, &peak_writes);
    if (!valid && header.work_profile &&
        (AFX_WORK_PROFILE_COMMANDS(header.work_profile) != peak_commands ||
         AFX_WORK_PROFILE_WRITES(header.work_profile) != peak_writes)) valid = AFX_BAD_FORMAT;
    if (valid) { (void)afx_asset_free(asset); free(linked); free(checkpoints); return -(int)valid; }
    uint32_t index;
    (void)resolve_asset(asset, &index);
    g_assets[index].header = header;
    g_assets[index].checkpoints = checkpoints;
    g_assets[index].checkpoints_size = header.checkpoints_size;
    g_assets[index].image = linked;
    g_assets[index].image_owned = true;
    g_assets[index].peak_commands = peak_commands;
    g_assets[index].peak_register_writes = peak_writes;
    *out = asset;
    return AFX_OK;
}
int afx_external_flow_begin(const afx_external_flow_t *recipe, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!g_ready || !recipe || !recipe->stream || !recipe->stream_size ||
        (recipe->setup_count && !recipe->setups) ||
        ((recipe->flags & AFX_FLAG_LANES) && !recipe->lane_map) ||
        recipe->setup_count > 65536 || recipe->setup_count > AFX_ASSET_LIMIT / AFX_SETUP_BYTES ||
        recipe->required_channels > AFX_MAX_FLOW_CHANNELS)
        return -AFX_BAD_BOUNDS;
    uint32_t lane_bytes = (recipe->flags & AFX_FLAG_LANES) ? recipe->required_channels : 0;
    uint32_t fixed_bytes = recipe->setup_count * AFX_SETUP_BYTES + lane_bytes;
    if (fixed_bytes > AFX_ASSET_LIMIT || recipe->stream_size > AFX_ASSET_LIMIT - fixed_bytes)
        return -AFX_BAD_BOUNDS;
    afx_file_header_t header = {
        .magic = AFX_FILE_MAGIC, .abi = AFX_ABI_VERSION, .flags = recipe->flags,
        .image_size = recipe->setup_count * AFX_SETUP_BYTES + lane_bytes + recipe->stream_size,
        .setup_count = recipe->setup_count,
        .stream_offset = recipe->setup_count * AFX_SETUP_BYTES + lane_bytes,
        .stream_size = recipe->stream_size, .required_channels = recipe->required_channels,
        .tick_rate_num = recipe->tick_rate_num, .tick_rate_den = recipe->tick_rate_den
    };
    uint8_t *image = upload_image_alloc(header.image_size);
    afx_sample_t *samples = recipe->setup_count ? calloc(recipe->setup_count, sizeof(*samples)) : NULL;
    afx_asset_t *dependencies = recipe->setup_count ? malloc(recipe->setup_count * sizeof(*dependencies)) : NULL;
    int result = -AFX_NO_HOST_RAM;
    if (!image || (recipe->setup_count && (!samples || !dependencies))) goto failed;
    if (lane_bytes) memcpy(image + recipe->setup_count * AFX_SETUP_BYTES, recipe->lane_map, lane_bytes);
    memcpy(image + header.stream_offset, recipe->stream, recipe->stream_size);
    for (uint32_t i = 0; i < recipe->setup_count; ++i) {
        const afx_sfx_setup_t *setup = &recipe->setups[i];
        uint16_t state[AFX_FIELD_COUNT];
        memcpy(state, setup->fields, sizeof(state));
        result = -AFX_BAD_SAMPLE;
        if ((state[0] & 0x1ffu) || state[1]) goto failed;
        if (state[0] & 0x400u) {
            if (setup->sample || setup->byte_offset) goto failed;
        } else {
            uint32_t index;
            if (!resolve_asset(setup->sample, &index) || !g_assets[index].sample) {
                result = -AFX_INVALID_HANDLE; goto failed;
            }
            const afx_asset_slot_t *sample = &g_assets[index];
            if (setup->byte_offset >= sample->size ||
                (sample->sample_format == AFX_PCM16 && (setup->byte_offset & 1u)) ||
                (sample->sample_format == AFX_ADPCM && setup->byte_offset)) goto failed;
            uint32_t address = sample->addr + setup->byte_offset;
            state[0] |= (uint16_t)((sample->sample_format << 7) | (address >> 16));
            state[1] = (uint16_t)address;
            uint32_t j = 0;
            while (j < header.sample_count && dependencies[j] != setup->sample) ++j;
            if (j == header.sample_count) {
                dependencies[j] = setup->sample;
                samples[j] = (afx_sample_t){sample->addr, sample->size, sample->sample_frames, sample->sample_format};
                ++header.sample_count;
            }
        }
        for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field)
            afx_write16(image + i * AFX_SETUP_BYTES + field * 2, state[field]);
    }
    result = -(int)afx_sfx_validate(image, &header, samples);
    if (result) goto failed;
    uint32_t commands, writes;
    result = -(int)profile_stream(&header, image, &commands, &writes);
    if (result) goto failed;
    afx_asset_t asset = reserve_asset(header.image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) { result = -AFX_NO_AICA_RAM; goto failed; }
    uint32_t index;
    (void)resolve_asset(asset, &index);
    afx_asset_slot_t *flow = &g_assets[index];
    flow->sfx = !(header.flags & AFX_FLAG_MUSIC);
    flow->header = header;
    flow->image = image;
    flow->image_owned = true;
    flow->dependencies = dependencies;
    flow->dependency_count = header.sample_count;
    flow->peak_commands = commands;
    flow->peak_register_writes = writes;
    for (uint32_t i = 0; i < header.sample_count; ++i) {
        uint32_t dependency;
        (void)resolve_asset(dependencies[i], &dependency);
        ++g_assets[dependency].references;
    }
    free(samples);
    *out = asset;
    return AFX_OK;
failed:
    free(image); free(samples); free(dependencies);
    return result;
}

int afx_flow_upload_step(afx_asset_t asset, uint32_t max_bytes,
                         uint32_t *out_uploaded, bool *out_complete) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (out_uploaded) *out_uploaded = 0;
    if (out_complete) *out_complete = false;
    if (!max_bytes || !resolve_asset(asset, &index) || !g_assets[index].uploading)
        return -AFX_BAD_BOUNDS;
    afx_asset_slot_t *slot = &g_assets[index];
    uint32_t uploaded = slot->upload_cursor;
    uint32_t remaining = slot->size - uploaded;
    uint32_t amount = max_bytes > AFX_UPLOAD_STEP_BYTES ? AFX_UPLOAD_STEP_BYTES : max_bytes;
    if (amount > remaining) amount = remaining;
    if (amount < remaining) amount &= ~3u;
    if (!amount || afx_mem_upload(slot->addr + uploaded, slot->image + uploaded, amount))
        return -AFX_BAD_BOUNDS;
    slot->upload_cursor += amount;
    if (out_uploaded) *out_uploaded = amount;
    if (slot->upload_cursor == slot->size) {
        slot->uploading = false;
        slot->live = true;
        slot->flow = true;
        if (out_complete) *out_complete = true;
    }
    return AFX_OK;
}
int afx_flow_upload_dma_step(afx_asset_t asset, uint32_t max_bytes,
                             uint32_t *out_uploaded, bool *out_complete) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (out_uploaded) *out_uploaded = 0;
    if (out_complete) *out_complete = false;
    if (!max_bytes || !resolve_asset(asset, &index) || !g_assets[index].uploading)
        return -AFX_BAD_BOUNDS;
    afx_asset_slot_t *slot = &g_assets[index];
    if (g_upload_dma_asset) {
        if (g_upload_dma_asset != asset) return -AFX_BUSY;
        if (!g_upload_dma_done) return AFX_OK;
        uint32_t uploaded = g_upload_dma_bytes;
        upload_dma_wait();
        slot->upload_cursor += uploaded;
        if (out_uploaded) *out_uploaded = uploaded;
    }
    if (slot->upload_cursor == slot->size) {
        slot->uploading = false;
        slot->live = slot->flow = true;
        if (out_complete) *out_complete = true;
        return AFX_OK;
    }
    uint32_t remaining = slot->size - slot->upload_cursor;
    uint32_t amount = remaining < max_bytes ? remaining : max_bytes;
    if (amount < remaining) amount &= ~(AFX_UPLOAD_ALIGN - 1u);
    if (!amount) return -AFX_BAD_BOUNDS;
    /* Assets and staged sources are aligned and their padding is zeroed, so the
     * final DMA includes only harmless local padding rather than using PIO. */
    uint32_t dma_bytes = amount == remaining ? align_up(amount, AFX_UPLOAD_ALIGN) : amount;
    dcache_wback_range((uintptr_t)slot->image + slot->upload_cursor, dma_bytes);
    g_upload_dma_asset = asset;
    g_upload_dma_bytes = amount;
    g_upload_dma_stream = false;
    g_upload_dma_done = false;
    if (spu_dma_transfer(slot->image + slot->upload_cursor,
                         slot->addr + slot->upload_cursor, dma_bytes, 0,
                         upload_dma_complete, NULL)) {
        g_upload_dma_asset = AFX_ASSET_INVALID;
        g_upload_dma_bytes = 0;
        return -AFX_BUSY;
    }
    return AFX_OK;
}
static int finish_flow_upload(afx_asset_t asset, afx_asset_t *out) {
    bool complete = false;
    while (!complete) {
        int result = afx_flow_upload_step(asset, AFX_UPLOAD_STEP_BYTES, NULL, &complete);
        if (result) { (void)afx_asset_free(asset); return result; }
    }
    *out = asset;
    return AFX_OK;
}
int afx_flow_upload(const void *data, uint32_t size, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    afx_asset_t asset;
    if (!out) return -AFX_BAD_BOUNDS;
    int result = afx_flow_upload_begin(data, size, &asset);
    return result ? result : finish_flow_upload(asset, out);
}
int afx_flow_upload_inplace(void *data, uint32_t size, afx_asset_t *out) {
    afx_file_header_t header;
    uint8_t *checkpoints = NULL;
    uint32_t commands = 0, writes = 0, index;
    afx_asset_t asset;
    afx_result_t valid;
    int result;

    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    valid = afx_file_validate(data, size, &header);
    if (valid) return -(int)valid;
    if (header.checkpoints_size) {
        checkpoints = malloc(header.checkpoints_size);
        if (!checkpoints) return -AFX_NO_HOST_RAM;
        memcpy(checkpoints, (const uint8_t *)data + header.checkpoints_offset, header.checkpoints_size);
    }
    asset = reserve_asset(header.image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) { free(checkpoints); return -AFX_NO_AICA_RAM; }
    valid = afx_link_validated_image_inplace(data, &header, afx_asset_addr(asset), size);
    if (!valid) valid = profile_stream(&header, data, &commands, &writes);
    if (!valid && header.work_profile &&
        (AFX_WORK_PROFILE_COMMANDS(header.work_profile) != commands ||
         AFX_WORK_PROFILE_WRITES(header.work_profile) != writes)) valid = AFX_BAD_FORMAT;
    if (valid) { (void)afx_asset_free(asset); free(checkpoints); return -(int)valid; }
    (void)resolve_asset(asset, &index);
    g_assets[index].header = header;
    g_assets[index].checkpoints = checkpoints;
    g_assets[index].checkpoints_size = header.checkpoints_size;
    g_assets[index].image = data;
    g_assets[index].peak_commands = commands;
    g_assets[index].peak_register_writes = writes;
    result = finish_flow_upload(asset, out);
    return result;
}

int afx_flow_stream_begin(const afx_file_header_t *header, const void *checkpoints,
                          afx_asset_t *out) {
    uint32_t index;
    afx_asset_t asset;

    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!header || !header->image_size || !header->stream_size ||
        header->stream_offset > header->image_size ||
        header->stream_size > header->image_size - header->stream_offset ||
        !header->required_channels || header->required_channels > AFX_MAX_FLOW_CHANNELS ||
        !header->tick_rate_num || !header->tick_rate_den ||
        (header->checkpoints_size && !checkpoints)) return -AFX_BAD_BOUNDS;
    asset = reserve_asset(header->image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) return -AFX_NO_AICA_RAM;
    (void)resolve_asset(asset, &index);
    g_assets[index].header = *header;
    if (header->checkpoints_size) {
        g_assets[index].checkpoints = malloc(header->checkpoints_size);
        if (!g_assets[index].checkpoints) { (void)afx_asset_free(asset); return -AFX_NO_HOST_RAM; }
        memcpy(g_assets[index].checkpoints, checkpoints, header->checkpoints_size);
        g_assets[index].checkpoints_size = header->checkpoints_size;
    }
    *out = asset;
    return AFX_OK;
}
int afx_flow_stream_upload(afx_asset_t asset, uint32_t image_offset,
                           const void *data, uint32_t size) {
    uint32_t index;

    HOST_GUARD(-AFX_BUSY);
    if (!data || !size || !resolve_asset(asset, &index) || g_assets[index].sample_bank ||
        !g_assets[index].uploading)
        return -AFX_BAD_BOUNDS;
    afx_asset_slot_t *flow = &g_assets[index];
    if (image_offset != flow->upload_cursor || size > flow->size - image_offset)
        return -AFX_BAD_BOUNDS;
    int result = afx_mem_upload(flow->addr + image_offset, data, size);
    if (result) return result;
    flow->upload_cursor += size;
    return AFX_OK;
}
int afx_flow_stream_upload_dma_begin(afx_asset_t asset, uint32_t image_offset,
                                     const void *data, uint32_t size) {
    uint32_t index, padded;

    HOST_GUARD(-AFX_BUSY);
    if (!data || !size || ((uintptr_t)data & (AFX_UPLOAD_ALIGN - 1u)) ||
        g_upload_dma_asset || !resolve_asset(asset, &index) || g_assets[index].sample_bank ||
        !g_assets[index].uploading)
        return -AFX_BAD_BOUNDS;
    afx_asset_slot_t *flow = &g_assets[index];
    padded = align_up(size, AFX_UPLOAD_ALIGN);
    if (!padded || image_offset != flow->upload_cursor || size > flow->size - image_offset ||
        padded > flow->allocation_size - image_offset) return -AFX_BAD_BOUNDS;
    dcache_wback_range((uintptr_t)data, padded);
    g_upload_dma_asset = asset;
    g_upload_dma_bytes = size;
    g_upload_dma_offset = image_offset;
    g_upload_dma_stream = true;
    g_upload_dma_done = false;
    if (spu_dma_transfer((void *)data, flow->addr + image_offset, padded, 0,
                         upload_dma_complete, NULL)) {
        g_upload_dma_asset = AFX_ASSET_INVALID;
        g_upload_dma_bytes = 0;
        g_upload_dma_offset = 0;
        g_upload_dma_stream = false;
        return -AFX_BUSY;
    }
    return AFX_OK;
}
int afx_flow_stream_upload_dma_poll(afx_asset_t asset, bool *out_complete) {
    uint32_t index;

    HOST_GUARD(-AFX_BUSY);
    if (out_complete) *out_complete = false;
    if (!resolve_asset(asset, &index) || g_assets[index].sample_bank ||
        !g_assets[index].uploading ||
        g_upload_dma_asset != asset || !g_upload_dma_stream) return -AFX_BAD_BOUNDS;
    if (!g_upload_dma_done) return AFX_OK;
    afx_asset_slot_t *flow = &g_assets[index];
    if (flow->upload_cursor != g_upload_dma_offset) return -AFX_BAD_BOUNDS;
    flow->upload_cursor += g_upload_dma_bytes;
    upload_dma_wait();
    if (out_complete) *out_complete = true;
    return AFX_OK;
}
int afx_flow_stream_finish(afx_asset_t asset) {
    uint32_t index;

    HOST_GUARD(-AFX_BUSY);
    if (!resolve_asset(asset, &index) || g_assets[index].sample_bank ||
        !g_assets[index].uploading) return -AFX_BAD_BOUNDS;
    if (g_upload_dma_asset == asset) return -AFX_BUSY;
    afx_asset_slot_t *flow = &g_assets[index];
    if (flow->upload_cursor != flow->size) return -AFX_BAD_BOUNDS;
    flow->peak_commands = AFX_WORK_PROFILE_COMMANDS(flow->header.work_profile);
    flow->peak_register_writes = AFX_WORK_PROFILE_WRITES(flow->header.work_profile);
    flow->uploading = false;
    flow->live = flow->flow = true;
    return AFX_OK;
}

int afx_flow_release_host_image(afx_asset_t asset) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_asset(asset, &index) || !g_assets[index].flow) return -AFX_INVALID_HANDLE;
    afx_asset_slot_t *flow = &g_assets[index];
    if (flow->uploading) return -AFX_BUSY;
    if (flow->image_owned) free(flow->image);
    flow->image = NULL;
    flow->image_owned = false;
    return AFX_OK;
}
int afx_external_flow_upload(const afx_external_flow_t *recipe, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    afx_asset_t asset;
    if (!out) return -AFX_BAD_BOUNDS;
    int result = afx_external_flow_begin(recipe, &asset);
    return result ? result : finish_flow_upload(asset, out);
}

int afx_sfx_flow_begin(const afx_sfx_flow_t *recipe, afx_asset_t *out) {
    if (!recipe || recipe->flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_LANES)) return -AFX_BAD_COMMAND;
    return afx_external_flow_begin(recipe, out);
}

int afx_sfx_flow_upload(const afx_sfx_flow_t *recipe, afx_asset_t *out) {
    if (!recipe || recipe->flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_LANES)) return -AFX_BAD_COMMAND;
    return afx_external_flow_upload(recipe, out);
}

