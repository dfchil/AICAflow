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
    case AFX_OP_KEYOFF: case AFX_OP_FREE: prefix = 2; tmp[1] = e->channel; break;
    case AFX_OP_NEXT: prefix = 2; tmp[1] = (uint8_t)e->join; break;
    case AFX_OP_PATCH_LEVEL: case AFX_OP_PATCH_ENV: case AFX_OP_PATCH_ENV_LEVEL:
        if (afx_compact_opcode(AFX_OP_PATCH, e->mask) != e->opcode || !values) return AFX_BAD_COMMAND;
        prefix = 2; tmp[1] = e->channel;
        for (uint32_t i = 0; i < afx_field_value_bytes(e->mask) / 2; ++i)
            afx_write16(tmp + prefix + 2 * i, values[i]);
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
    n = prefix + ((e->opcode >= AFX_OP_NOTE && e->opcode <= AFX_OP_PATCH) || e->opcode == AFX_OP_NOTE_PL ||
                  e->opcode == AFX_OP_PATCH_LEVEL || e->opcode == AFX_OP_PATCH_ENV ||
                  e->opcode == AFX_OP_PATCH_ENV_LEVEL ? afx_field_value_bytes(e->mask) : 0);
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
        h->reserved ||
        h->relocation_count != h->setup_count ||
        (h->flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_MUSIC | AFX_FLAG_MUSIC_CHORUS |
                      AFX_FLAG_LANES | AFX_FLAG_BANKS))) return AFX_BAD_BOUNDS;
    if (!(h->bank_id_low || h->bank_id_high) && h->relocation_count) return AFX_BAD_SAMPLE;
    image = file + h->image_offset;
    uint32_t slots = 1;
    if (h->flags & AFX_FLAG_BANKS) {
        /* The bank list lies with the relocations before the image; the bank
         * table ends the image, zero until the loader fills it. */
        uint32_t bank_table = AFX_BANK_TABLE_OFFSET(h->stream_offset + h->stream_size);
        if (!(h->bank_id_low || h->bank_id_high) ||
            !table(h->banks_offset, 1, 4, h->image_offset)) return AFX_BAD_BOUNDS;
        slots = 1u + afx_read32(file + h->banks_offset);
        if (slots < 2 || slots > AFX_MAX_FLOW_BANKS ||
            !table(h->banks_offset + 4u, slots - 1u, 8, h->image_offset) ||
            bank_table + AFX_BANK_TABLE_BYTES != h->image_size) return AFX_BAD_BOUNDS;
        for (uint32_t slot = 1; slot < slots; ++slot)
            if (!(afx_read32(file + h->banks_offset + slot * 8u - 4u) ||
                  afx_read32(file + h->banks_offset + slot * 8u))) return AFX_BAD_SAMPLE;
        for (uint32_t i = 0; i < AFX_BANK_TABLE_BYTES; ++i)
            if (image[bank_table + i]) return AFX_BAD_FORMAT;
    } else if (h->banks_offset) return AFX_BAD_BOUNDS;
    for (uint32_t i = 0; i < h->relocation_count; ++i) {
        afx_relocation_t r = relocation_at(file, h, i);
        uint16_t control;
        uint32_t address;
        if (r.pair_offset != i * AFX_SETUP_BYTES || !r.byte_offset) return AFX_BAD_RELOCATION;
        control = afx_read16(image + r.pair_offset);
        address = ((uint32_t)(control & 0x7fu) << 16) | afx_read16(image + r.pair_offset + 2);
        if ((control & 0x400u) || address != r.sample_index) return AFX_BAD_RELOCATION;
        if ((h->flags & AFX_FLAG_BANKS) && (address >> AFX_BANK_SLOT_SHIFT) >= slots)
            return AFX_BAD_RELOCATION;
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

/* The work of one event, towards the burst between two waits. */
static void event_work(const afx_event_t *event, uint32_t *commands, uint32_t *writes) {
    if (event->opcode == AFX_OP_NOTE) {
        ++*commands; *writes += 19;
    } else if (event->opcode == AFX_OP_PATCH) {
        ++*commands; *writes += afx_field_value_bytes(event->mask) / 2u;
    } else if (event->opcode == AFX_OP_KEYOFF || event->opcode == AFX_OP_FREE) {
        ++*commands; ++*writes;
    }
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
        }
        event_work(&event, &commands, &writes);
        if (event.opcode == AFX_OP_NOTE && event.setup >= h->setup_count) return AFX_BAD_COMMAND;
        if (event.opcode >= AFX_OP_NOTE && event.opcode <= AFX_OP_KEYOFF &&
            event.channel >= h->required_channels) return AFX_BAD_COMMAND;
        /* NEXT and FREE belong to songs. */
        if (event.opcode == AFX_OP_NEXT || event.opcode == AFX_OP_FREE) return AFX_BAD_COMMAND;
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

uint32_t afx_file_bank_count(const void *data, const afx_file_header_t *header) {
    if (!(header->flags & AFX_FLAG_BANKS)) return header->bank_id_low || header->bank_id_high;
    return 1u + afx_read32((const uint8_t *)data + header->banks_offset);
}

void afx_file_bank_identity(const void *data, const afx_file_header_t *header, uint32_t slot,
                            uint32_t *out_low, uint32_t *out_high) {
    const uint8_t *entry = (const uint8_t *)data + header->banks_offset + slot * 8u - 4u;
    *out_low = slot ? afx_read32(entry) : header->bank_id_low;
    *out_high = slot ? afx_read32(entry + 4) : header->bank_id_high;
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

afx_scene_entry_t afx_scene_entry_at(const void *data, uint32_t i) {
    afx_scene_entry_t e;
    read_words(&e, (const uint8_t *)data + AFX_SCENE_HEADER_BYTES + i * AFX_SCENE_ENTRY_BYTES, sizeof(e));
    return e;
}

const char *afx_scene_entry_name(const void *data, const afx_scene_entry_t *entry) {
    return entry->name ? (const char *)data + entry->name : "";
}

afx_result_t afx_scene_validate(const void *data, uint32_t size, afx_scene_header_t *out) {
    const uint8_t *file = data;
    afx_scene_header_t h;
    if (!data || size < sizeof(h)) return AFX_BAD_BOUNDS;
    read_words(&h, file, sizeof(h));
    if (h.magic != AFX_SCENE_MAGIC || h.version != AFX_SCENE_VERSION ||
        !(h.id_low || h.id_high) || h.reserved) return AFX_BAD_FORMAT;
    if (h.total_size != size || !h.entry_count ||
        h.entry_count > (size - AFX_SCENE_HEADER_BYTES) / AFX_SCENE_ENTRY_BYTES ||
        !h.block_bytes || (h.block_bytes & 31u)) return AFX_BAD_BOUNDS;
    uint32_t names = AFX_SCENE_HEADER_BYTES + h.entry_count * AFX_SCENE_ENTRY_BYTES, rings = 0;
    for (uint32_t i = 0; i < h.entry_count; ++i) {
        afx_scene_entry_t e = afx_scene_entry_at(data, i);
        int stored = e.kind == AFX_SCENE_BANK || e.kind == AFX_SCENE_FLOW || e.kind == AFX_SCENE_SONG;
        uint32_t header = e.kind == AFX_SCENE_BANK ? AFX_BANK_HEADER_BYTES :
                          e.kind == AFX_SCENE_SONG ? AFX_SONG_HEADER_BYTES : AFX_FILE_HEADER_BYTES;
        uint32_t align = e.kind == AFX_SCENE_RING ? AFX_SCENE_RING_ALIGN : 32u;
        if (e.kind < AFX_SCENE_BANK || e.kind > AFX_SCENE_SONG || e.reserved ||
            (e.ticks && e.kind != AFX_SCENE_FLOW && e.kind != AFX_SCENE_SONG) ||
            (e.kind == AFX_SCENE_RING && rings++))
            return AFX_BAD_FORMAT;
        if (stored ? e.file_size < header || e.file_offset < names ||
                     !afx_range(e.file_offset, e.file_size, size)
                   : e.file_offset || e.file_size) return AFX_BAD_BOUNDS;
        if (!e.block_bytes || (e.block_bytes & 31u) || (e.block_offset & (align - 1u)) ||
            !afx_range(e.block_offset, e.block_bytes, h.block_bytes)) return AFX_BAD_BOUNDS;
        if (e.kind == AFX_SCENE_RING) {
            uint32_t rbl = 0;
            while (rbl < 3 && e.block_bytes != AFX_SCENE_RING_MIN_BYTES << rbl) ++rbl;
            if (e.block_bytes != AFX_SCENE_RING_MIN_BYTES << rbl) return AFX_BAD_BOUNDS;
        }
        if (e.name && (e.name < names || e.name >= size || !memchr(file + e.name, 0, size - e.name)))
            return AFX_BAD_BOUNDS;
        if (e.name && !file[e.name]) return AFX_BAD_FORMAT;
        for (uint32_t j = 0; j < i; ++j) {
            afx_scene_entry_t f = afx_scene_entry_at(data, j);
            if (e.block_offset < f.block_offset + f.block_bytes &&
                f.block_offset < e.block_offset + e.block_bytes) return AFX_BAD_BOUNDS;
            if (e.name && f.name && !strcmp(afx_scene_entry_name(data, &e), afx_scene_entry_name(data, &f)))
                return AFX_BAD_FORMAT;
        }
    }
    if (out) *out = h;
    return AFX_OK;
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

/* Songs (AFM). */

static int song_table(uint32_t offset, uint32_t count, uint32_t stride, uint32_t end) {
    if (!count) return offset == 0;
    return !(offset & 3) && offset >= AFX_SONG_HEADER_BYTES &&
           offset <= end && count <= (end - offset) / stride;
}

afx_song_section_t afx_song_section(const void *image, const afx_song_image_header_t *ih, uint32_t i) {
    const uint8_t *entry = (const uint8_t *)image + ih->sections_offset + i * AFX_SONG_SECTION_BYTES;
    return (afx_song_section_t){afx_read32(entry), afx_read32(entry + 4), afx_read32(entry + 8)};
}
uint32_t afx_song_order(const void *image, const afx_song_image_header_t *ih, uint32_t position) {
    return afx_read16((const uint8_t *)image + ih->order_offset + position * 2u);
}

afx_result_t afx_song_validate_layout(const void *data, uint32_t size, afx_song_header_t *out,
                                      afx_song_image_header_t *out_image) {
    const uint8_t *file = data;
    afx_song_header_t h;
    afx_song_image_header_t ih;
    if (!data || size < sizeof(h)) return AFX_BAD_BOUNDS;
    read_words(&h, file, sizeof(h));
    if (h.magic != AFX_SONG_MAGIC || h.version != AFX_SONG_VERSION) return AFX_BAD_FORMAT;
    for (uint32_t i = 0; i < 7; ++i) if (h.reserved[i]) return AFX_BAD_FORMAT;
    if (h.total_size != size || h.image_offset < AFX_SONG_HEADER_BYTES || (h.image_offset & 31u) ||
        h.image_size < AFX_SONG_IMAGE_HEADER_BYTES || h.image_offset + h.image_size != size ||
        !(h.flags & AFX_FLAG_MUSIC) ||
        (h.flags & ~(AFX_FLAG_MUSIC | AFX_FLAG_LANES | AFX_FLAG_BANKS)) ||
        !h.channels || h.channels > AFX_MAX_FLOW_CHANNELS || h.voices > AFX_MAX_FLOW_CHANNELS ||
        !h.tick_rate_num || !h.tick_rate_den || !h.control_id ||
        h.setup_count > (h.image_size - AFX_SONG_IMAGE_HEADER_BYTES) / AFX_SETUP_BYTES ||
        !song_table(h.relocations_offset, h.setup_count, 12, h.image_offset)) return AFX_BAD_BOUNDS;
    if (!(h.bank_id_low || h.bank_id_high) && h.setup_count) return AFX_BAD_SAMPLE;
    const uint8_t *image = file + h.image_offset;
    read_words(&ih, image, sizeof(ih));
    uint32_t setups_end = AFX_SONG_IMAGE_HEADER_BYTES + h.setup_count * AFX_SETUP_BYTES;
    uint32_t lanes_end = setups_end + ((h.flags & AFX_FLAG_LANES) ? ((h.channels + 3u) & ~3u) : 0);
    uint32_t streams_end = ih.streams_offset + ih.streams_size;
    uint32_t image_end = (h.flags & AFX_FLAG_BANKS) ? AFX_BANK_TABLE_OFFSET(streams_end) + AFX_BANK_TABLE_BYTES
                                                    : streams_end;
    if (ih.channels != h.channels || !ih.section_count || !ih.order_count || ih.order_count > AFX_SONG_MAX_ORDER ||
        (ih.loop_index != AFX_SONG_NO_LOOP && ih.loop_index >= ih.order_count) ||
        (ih.sections_offset & 3u) || (ih.order_offset & 3u) || ih.sections_offset < lanes_end ||
        ih.section_count > (h.image_size - ih.sections_offset) / AFX_SONG_SECTION_BYTES ||
        ih.order_offset < ih.sections_offset + ih.section_count * AFX_SONG_SECTION_BYTES ||
        ih.order_count > (h.image_size - ih.order_offset) / 2u ||
        ih.streams_offset < ih.order_offset + ih.order_count * 2u || !ih.streams_size ||
        streams_end < ih.streams_offset || streams_end > h.image_size || image_end != h.image_size)
        return AFX_BAD_BOUNDS;
    for (uint32_t i = 0; i < ih.section_count; ++i) {
        afx_song_section_t section = afx_song_section(image, &ih, i);
        if (section.stream_offset < ih.streams_offset || !section.stream_size ||
            section.stream_size > AFX_SONG_MAX_STREAM ||
            !afx_range(section.stream_offset, section.stream_size, streams_end) || !section.body_ticks)
            return AFX_BAD_BOUNDS;
    }
    for (uint32_t i = 0; i < ih.order_count; ++i)
        if (afx_song_order(image, &ih, i) >= ih.section_count) return AFX_BAD_BOUNDS;
    uint32_t slots = 1;
    if (h.flags & AFX_FLAG_BANKS) {
        if (!(h.bank_id_low || h.bank_id_high) || !song_table(h.banks_offset, 1, 4, h.image_offset))
            return AFX_BAD_BOUNDS;
        slots = 1u + afx_read32(file + h.banks_offset);
        if (slots < 2 || slots > AFX_MAX_FLOW_BANKS ||
            !song_table(h.banks_offset + 4u, slots - 1u, 8, h.image_offset)) return AFX_BAD_BOUNDS;
        for (uint32_t slot = 1; slot < slots; ++slot)
            if (!(afx_read32(file + h.banks_offset + slot * 8u - 4u) ||
                  afx_read32(file + h.banks_offset + slot * 8u))) return AFX_BAD_SAMPLE;
        for (uint32_t i = 0; i < AFX_BANK_TABLE_BYTES; ++i)
            if (image[AFX_BANK_TABLE_OFFSET(streams_end) + i]) return AFX_BAD_FORMAT;
    } else if (h.banks_offset) return AFX_BAD_BOUNDS;
    for (uint32_t i = 0; i < h.setup_count; ++i) {
        afx_relocation_t r;
        read_words(&r, file + h.relocations_offset + i * sizeof(r), sizeof(r));
        uint32_t pair = AFX_SONG_IMAGE_HEADER_BYTES + i * AFX_SETUP_BYTES;
        if (r.pair_offset != pair || !r.byte_offset) return AFX_BAD_RELOCATION;
        uint16_t control = afx_read16(image + pair);
        uint32_t address = ((uint32_t)(control & 0x7fu) << 16) | afx_read16(image + pair + 2);
        if ((control & 0x400u) || address != r.sample_index) return AFX_BAD_RELOCATION;
        if ((h.flags & AFX_FLAG_BANKS) && (address >> AFX_BANK_SLOT_SHIFT) >= slots)
            return AFX_BAD_RELOCATION;
    }
    if (h.flags & AFX_FLAG_LANES)
        for (uint32_t i = 0; i < h.channels; ++i)
            if (image[setups_end + i] >= AFX_MAX_FLOW_CHANNELS) return AFX_BAD_FORMAT;
    if (out) *out = h;
    if (out_image) *out_image = ih;
    return AFX_OK;
}

uint32_t afx_song_bank_count(const void *data, const afx_song_header_t *header) {
    if (!(header->flags & AFX_FLAG_BANKS)) return header->bank_id_low || header->bank_id_high;
    return 1u + afx_read32((const uint8_t *)data + header->banks_offset);
}

/* One executor of the simulated song. */
typedef struct {
    int active;
    uint32_t cursor, end; /* Image offsets in the current section's stream. */
    uint64_t due, ended; /* The tick its next instruction runs; the tick it last ended. */
    uint64_t mapped; /* Local channels holding a pool voice. */
} song_exec_t;

static uint32_t popcount64(uint64_t value) {
    uint32_t count = 0;
    for (; value; value &= value - 1u) ++count;
    return count;
}

/* Checks each section's stream on its own: instructions, setups, channels,
 * one NEXT at the body length with only END after a continue join, END last.
 * Returns the channels used and whether any section joins with AFX_JOIN_NEW. */
static afx_result_t song_sections_validate(const uint8_t *image, const afx_song_header_t *h,
                                           const afx_song_image_header_t *ih,
                                           uint32_t *out_channels, int *out_pool) {
    uint32_t channels = 0;
    int pool = 0;
    for (uint32_t i = 0; i < ih->section_count; ++i) {
        afx_song_section_t section = afx_song_section(image, ih, i);
        uint32_t cursor = section.stream_offset, end = cursor + section.stream_size, nexts = 0;
        uint64_t tick = 0;
        int terminal = 0, join = AFX_JOIN_NEW;
        while (cursor < end) {
            afx_event_t event;
            afx_result_t result = afx_decode_event(image + cursor, end - cursor, &event);
            if (result) return result;
            cursor += event.bytes;
            if (nexts && join == AFX_JOIN_CONTINUE && event.opcode != AFX_OP_END) return AFX_BAD_COMMAND;
            if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) tick += event.wait;
            else if (event.opcode == AFX_OP_NOTE && event.setup >= h->setup_count) return AFX_BAD_COMMAND;
            else if (event.opcode == AFX_OP_PARK) return AFX_BAD_COMMAND;
            else if (event.opcode == AFX_OP_NEXT) {
                if (nexts++ || tick != section.body_ticks) return AFX_BAD_COMMAND;
                join = (int)event.join;
                pool |= join == AFX_JOIN_NEW;
            } else if (event.opcode == AFX_OP_END) {
                if (cursor != end) return AFX_BAD_COMMAND;
                terminal = 1;
            }
            if (((event.opcode >= AFX_OP_NOTE && event.opcode <= AFX_OP_KEYOFF) ||
                 event.opcode == AFX_OP_FREE) && event.channel + 1u > channels)
                channels = event.channel + 1u;
        }
        if (!terminal || nexts != 1) return AFX_BAD_COMMAND;
    }
    *out_channels = channels ? channels : 1;
    *out_pool = pool;
    return AFX_OK;
}

/* Plays the order through as the firmware does, from the start through one
 * more pass of the looped part, and reports the peak work in a tick (both
 * executors together) and the most pool voices held at once. A new-play join
 * whose other executor has not ended in an earlier tick is refused. */
static afx_result_t song_play(const uint8_t *image, const afx_song_image_header_t *ih, int pool,
                              afx_validation_profile_t *work, uint32_t *out_voices) {
    song_exec_t exec[2] = {{0}};
    afx_song_section_t first = afx_song_section(image, ih, afx_song_order(image, ih, 0));
    uint32_t position = 0, voices = 0, joins = 0;
    uint32_t bound = ih->order_count +
        (ih->loop_index == AFX_SONG_NO_LOOP ? 0 : ih->order_count - ih->loop_index + 1u);
    exec[0] = (song_exec_t){1, first.stream_offset, first.stream_offset + first.stream_size, 0, 0, 0};
    *work = (afx_validation_profile_t){0};
    for (;;) {
        uint64_t now = UINT64_MAX;
        for (int e = 0; e < 2; ++e) if (exec[e].active && exec[e].due < now) now = exec[e].due;
        if (now == UINT64_MAX || joins > bound) break;
        uint32_t commands = 0, writes = 0;
        for (int busy = 1; busy;) {
            busy = 0;
            for (int e = 0; e < 2; ++e) {
                song_exec_t *x = &exec[e];
                while (x->active && x->due == now) {
                    afx_event_t event;
                    busy = 1;
                    if (afx_decode_event(image + x->cursor, x->end - x->cursor, &event)) return AFX_BAD_COMMAND;
                    x->cursor += event.bytes;
                    if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) {
                        x->due += event.wait;
                        break;
                    }
                    event_work(&event, &commands, &writes);
                    if (event.opcode >= AFX_OP_NOTE && event.opcode <= AFX_OP_KEYOFF)
                        x->mapped |= 1ull << event.channel;
                    else if (event.opcode == AFX_OP_FREE) x->mapped &= ~(1ull << event.channel);
                    else if (event.opcode == AFX_OP_END) {
                        x->active = 0; x->ended = now; x->mapped = 0;
                    } else if (event.opcode == AFX_OP_NEXT) {
                        uint32_t next = position + 1u;
                        if (next == ih->order_count) next = ih->loop_index;
                        if (next == AFX_SONG_NO_LOOP) continue; /* Nothing follows: the tail plays on. */
                        if (++joins > bound) break;
                        position = next;
                        afx_song_section_t section = afx_song_section(image, ih, afx_song_order(image, ih, next));
                        if (event.join == AFX_JOIN_CONTINUE) {
                            x->cursor = section.stream_offset;
                            x->end = section.stream_offset + section.stream_size;
                        } else {
                            song_exec_t *other = &exec[1 - e];
                            /* Two executors: the other one's tail must be over. */
                            if (other->active || other->ended == now) return AFX_BAD_COMMAND;
                            *other = (song_exec_t){1, section.stream_offset,
                                                   section.stream_offset + section.stream_size, now, 0, 0};
                        }
                    }
                    if (pool) {
                        uint32_t held = popcount64(exec[0].mapped) + popcount64(exec[1].mapped);
                        if (held > voices) voices = held;
                    }
                }
            }
        }
        if (commands > work->peak_commands) work->peak_commands = commands;
        if (writes > work->peak_register_writes) work->peak_register_writes = writes;
    }
    *out_voices = voices;
    return AFX_OK;
}

afx_result_t afx_song_validate_profile(const void *data, uint32_t size, afx_song_header_t *out,
                                       afx_validation_profile_t *profile) {
    afx_song_header_t h;
    afx_song_image_header_t ih;
    afx_result_t result = afx_song_validate_layout(data, size, &h, &ih);
    if (result) return result;
    const uint8_t *image = (const uint8_t *)data + h.image_offset;
    uint32_t channels, voices;
    int pool;
    afx_validation_profile_t work;
    if ((result = song_sections_validate(image, &h, &ih, &channels, &pool))) return result;
    if (channels != h.channels) return AFX_BAD_COMMAND;
    if ((result = song_play(image, &ih, pool, &work, &voices))) return result;
    if (voices != h.voices) return AFX_BAD_COMMAND;
    if (out) *out = h;
    if (profile) *profile = work;
    return AFX_OK;
}
afx_result_t afx_song_validate(const void *data, uint32_t size, afx_song_header_t *out) {
    return afx_song_validate_profile(data, size, out, NULL);
}
