#include <aicaflow/codec.h>
#include <string.h>

uint32_t afx_control_id(const void *image, uint32_t size) {
    const uint8_t *bytes = image;
    uint32_t hash = 2166136261u;
    if (!bytes) return 0;
    for (uint32_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash ? hash : 1;
}

afx_result_t afx_encode_event(uint8_t *out, uint32_t capacity, const afx_event_t *e,
                              const uint16_t *values, uint32_t *written) {
    uint8_t tmp[8 + AFX_SETUP_BYTES] = {0};
    uint32_t prefix, n;
    afx_event_t decoded;
    if (!out || !e || !written || (e->mask & ~AFX_FIELD_MASK)) return AFX_BAD_COMMAND;
    tmp[0] = e->opcode;
    switch (e->opcode) {
    case AFX_OP_END: case AFX_OP_PARK: prefix = 1; break;
    case AFX_OP_WAIT8:
        if (e->wait > 255) return AFX_BAD_COMMAND;
        prefix = 2; tmp[1] = (uint8_t)e->wait; break;
    case AFX_OP_WAIT16:
        if (e->wait > 65535) return AFX_BAD_COMMAND;
        prefix = 3; afx_write16(tmp + 1, (uint16_t)e->wait); break;
    case AFX_OP_WAIT32: prefix = 5; afx_write32(tmp + 1, e->wait); break;
    case AFX_OP_KEYOFF: prefix = 2; tmp[1] = e->channel; break;
    case AFX_OP_PATCH_LEVEL:
        if (e->mask != (1u << AFX_FIELD_TOTAL_LEVEL) || !values) return AFX_BAD_COMMAND;
        prefix = 2; tmp[1] = e->channel; afx_write16(tmp + 2, values[0]);
        break;
    case AFX_OP_NOTE_PL:
        if (e->mask != AFX_NOTE_PL_MASK || !values) return AFX_BAD_COMMAND;
        prefix = 4; tmp[1] = e->channel; afx_write16(tmp + 2, e->setup);
        afx_write16(tmp + 4, values[0]); afx_write16(tmp + 6, values[1]);
        break;
    case AFX_OP_NOTE: case AFX_OP_PATCH:
        prefix = e->opcode == AFX_OP_NOTE ? 8 : 6;
        tmp[1] = e->channel;
        if (e->opcode == AFX_OP_NOTE) afx_write16(tmp + 2, e->setup);
        afx_write32(tmp + prefix - 4, e->mask);
        if (e->mask && !values) return AFX_BAD_BOUNDS;
        for (uint32_t i = 0; i < afx_field_value_bytes(e->mask) / 2; ++i)
            afx_write16(tmp + prefix + 2 * i, values[i]);
        break;
    default: return AFX_BAD_COMMAND;
    }
    n = prefix + ((e->opcode == AFX_OP_PATCH_LEVEL || e->opcode == AFX_OP_NOTE_PL || e->opcode == AFX_OP_NOTE || e->opcode == AFX_OP_PATCH) ? afx_field_value_bytes(e->mask) : 0);
    afx_result_t r = afx_decode_event(tmp, n, &decoded);
    if (r != AFX_OK) return r;
    if (capacity < n) return AFX_BAD_BOUNDS;
    memcpy(out, tmp, n);
    *written = n;
    return AFX_OK;
}

/* Structs describe layout, but explicit loads also work on unaligned inputs.
 * memcpy each word into the object avoids type-punning/struct-as-array UB. */
static void read_words(void *out, const uint8_t *p, uint32_t size) {
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t word = afx_read32(p + i);
        memcpy((uint8_t *)out + i, &word, 4);
    }
}
void afx_encode_header(uint8_t out[AFX_FILE_HEADER_BYTES], const afx_file_header_t *h) {
    for (uint32_t i = 0; i < sizeof(*h); i += 4) {
        uint32_t word;
        memcpy(&word, (const uint8_t *)h + i, 4);
        afx_write32(out + i, word);
    }
}

static int table(uint32_t offset, uint32_t count, uint32_t stride, uint32_t end) {
    if (!count) return offset == 0;
    return !(offset & 3) && offset >= AFX_FILE_HEADER_BYTES &&
           offset <= end && count <= (end - offset) / stride;
}

static afx_relocation_t relocation_at(const uint8_t *file, const afx_file_header_t *h, uint32_t i) {
    afx_relocation_t r;
    read_words(&r, file + h->relocations_offset + i * sizeof(r), sizeof(r));
    return r;
}

static afx_result_t bank_file_validate(const uint8_t *file, uint32_t size,
                                       const afx_file_header_t *h) {
    const uint8_t *image;
    if (h->total_size != size || h->image_offset < AFX_FILE_HEADER_BYTES ||
        (h->image_offset & 31u) || !h->image_size ||
        !afx_range(h->image_offset, h->image_size, size) ||
        h->image_offset + h->image_size != size || !h->stream_size ||
        !afx_range(h->stream_offset, h->stream_size, h->image_size) ||
        !h->control_id || h->setup_count > h->image_size / AFX_SETUP_BYTES ||
        h->stream_offset < h->setup_count * AFX_SETUP_BYTES ||
        !h->required_channels || h->required_channels > AFX_MAX_FLOW_CHANNELS ||
        !h->tick_rate_num || !h->tick_rate_den ||
        !table(h->relocations_offset, h->relocation_count, 12, h->image_offset) ||
        h->reserved0 || h->reserved1 ||
        h->relocation_count != h->setup_count ||
        (h->flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_MUSIC | AFX_FLAG_MUSIC_CHORUS |
                      AFX_FLAG_LANES))) return AFX_BAD_BOUNDS;
    if (!(h->bank_id_low || h->bank_id_high) && h->relocation_count) return AFX_BAD_SAMPLE;
    image = file + h->image_offset;
    for (uint32_t i = 0; i < h->relocation_count; ++i) {
        afx_relocation_t r = relocation_at(file, h, i);
        uint16_t control;
        uint32_t address;
        if (r.pair_offset != i * AFX_SETUP_BYTES || !r.byte_offset) return AFX_BAD_RELOCATION;
        control = afx_read16(image + r.pair_offset);
        address = ((uint32_t)(control & 0x7fu) << 16) | afx_read16(image + r.pair_offset + 2);
        if ((control & 0x400u) || address != r.sample_index) return AFX_BAD_RELOCATION;
    }
    if (h->flags & AFX_FLAG_LANES) {
        const uint8_t *lanes = image + h->setup_count * AFX_SETUP_BYTES;
        if (h->required_channels > h->stream_offset - h->setup_count * AFX_SETUP_BYTES)
            return AFX_BAD_BOUNDS;
        for (uint32_t i = 0; i < h->required_channels; ++i)
            if (lanes[i] >= AFX_MAX_FLOW_CHANNELS) return AFX_BAD_FORMAT;
    }
    return AFX_OK;
}

static afx_result_t stream_validate(const uint8_t *image, const afx_file_header_t *h,
                                     afx_validation_profile_t *profile) {
    uint32_t cursor = h->stream_offset, end = cursor + h->stream_size;
    int terminal = 0;
    uint32_t commands = 0, writes = 0;
    afx_validation_profile_t work = {0};
    while (cursor < end) {
        afx_event_t event;
        afx_result_t result = afx_decode_event(image + cursor, end - cursor, &event);
        if (result) return result;
        cursor += event.bytes;
        if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) {
            if (commands > work.peak_commands) work.peak_commands = commands;
            if (writes > work.peak_register_writes) work.peak_register_writes = writes;
            commands = writes = 0;
        } else if (event.opcode == AFX_OP_NOTE) {
            ++commands; writes += 19;
        } else if (event.opcode == AFX_OP_PATCH) {
            ++commands; writes += afx_field_value_bytes(event.mask) / 2u;
        } else if (event.opcode == AFX_OP_KEYOFF) {
            ++commands; ++writes;
        }
        if (event.opcode == AFX_OP_NOTE && event.setup >= h->setup_count) return AFX_BAD_COMMAND;
        if (event.opcode >= AFX_OP_NOTE && event.opcode <= AFX_OP_KEYOFF &&
            event.channel >= h->required_channels) return AFX_BAD_COMMAND;
        if (event.opcode == AFX_OP_END || event.opcode == AFX_OP_PARK) {
            if (cursor != end || (event.opcode == AFX_OP_PARK && !(h->flags & AFX_FLAG_CONTROLLED)))
                return AFX_BAD_COMMAND;
            terminal = 1;
        }
    }
    if (!terminal) return AFX_BAD_COMMAND;
    if (commands > work.peak_commands) work.peak_commands = commands;
    if (writes > work.peak_register_writes) work.peak_register_writes = writes;
    if (profile) *profile = work;
    return AFX_OK;
}

afx_result_t afx_file_validate_layout(const void *data, uint32_t size, afx_file_header_t *out) {
    const uint8_t *file = data;
    afx_file_header_t h;
    if (!data || size < sizeof(h)) return AFX_BAD_BOUNDS;
    read_words(&h, file, sizeof(h));
    if (h.magic != AFX_FILE_MAGIC || h.abi != AFX_FILE_VERSION) return AFX_BAD_FORMAT;
    afx_result_t result = bank_file_validate(file, size, &h);
    if (!result && out) *out = h;
    return result;
}

afx_result_t afx_file_validate_profile(const void *data, uint32_t size,
                                       afx_file_header_t *out, afx_validation_profile_t *profile) {
    afx_file_header_t h;
    afx_result_t result = afx_file_validate_layout(data, size, &h);
    if (!result) result = stream_validate((const uint8_t *)data + h.image_offset, &h, profile);
    if (!result && out) *out = h;
    return result;
}

afx_result_t afx_file_validate(const void *data, uint32_t size, afx_file_header_t *out) {
    return afx_file_validate_profile(data, size, out, NULL);
}

afx_result_t afx_flow_duration(const void *data, uint32_t size, uint64_t *out_ticks,
                               uint32_t *out_tick_rate_num, uint32_t *out_tick_rate_den) {
    afx_file_header_t header;
    if (!out_ticks || !out_tick_rate_num || !out_tick_rate_den) return AFX_BAD_BOUNDS;
    afx_result_t result = afx_file_validate(data, size, &header);
    if (result) return result;
    const uint8_t *stream = (const uint8_t *)data + header.image_offset + header.stream_offset;
    uint64_t ticks = 0;
    for (uint32_t cursor = 0; cursor < header.stream_size;) {
        afx_event_t event;
        result = afx_decode_event(stream + cursor, header.stream_size - cursor, &event);
        if (result) return result;
        cursor += event.bytes;
        if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) ticks += event.wait;
    }
    *out_ticks = ticks;
    *out_tick_rate_num = header.tick_rate_num;
    *out_tick_rate_den = header.tick_rate_den;
    return AFX_OK;
}
