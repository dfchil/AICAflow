#include "aicaflow_codec.h"
#include <string.h>

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

static int overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) {
    return an && bn && a < b + bn && b < a + an; /* Only on already bounded ranges. */
}
static int table(uint32_t offset, uint32_t count, uint32_t stride, uint32_t end) {
    if (!count) return offset == 0;
    return !(offset & 3) && offset >= AFX_FILE_HEADER_BYTES &&
           offset <= end && count <= (end - offset) / stride;
}
static afx_sample_t sample_at(const uint8_t *file, const afx_file_header_t *h, uint32_t i) {
    afx_sample_t s;
    read_words(&s, file + h->samples_offset + i * sizeof(s), sizeof(s));
    return s;
}
static afx_relocation_t relocation_at(const uint8_t *file, const afx_file_header_t *h, uint32_t i) {
    afx_relocation_t r;
    read_words(&r, file + h->relocations_offset + i * sizeof(r), sizeof(r));
    return r;
}
static uint32_t sample_address(const uint16_t *state) {
    return ((uint32_t)(state[0] & 0x7f) << 16) | state[1];
}
static afx_result_t validate_state(const uint8_t *file, const afx_file_header_t *h,
                                   const uint16_t *state, const afx_sample_t *external) {
    if (state[0] & 0x400) return AFX_OK; /* Hardware noise, no sample fetch. */
    uint32_t address = sample_address(state), format = (state[0] >> 7) & 3;
    for (uint32_t i = 0; i < h->sample_count; ++i) {
        afx_sample_t s = file ? sample_at(file, h, i) : external[i];
        if (address < s.image_offset || address - s.image_offset >= s.byte_size) continue;
        uint32_t offset = address - s.image_offset, skipped;
        if (format != s.format || (format == AFX_PCM16 && (offset & 1))) return AFX_BAD_SAMPLE;
        skipped = format == AFX_PCM16 ? offset / 2 : format == AFX_PCM8 ? offset : offset * 2;
        /* LEA is a sample index, NOT a byte length. Keep a real sample at LEA;
         * interpolation padding/pitch-specific guards are compiler work. */
        if (skipped >= s.frames || state[3] >= s.frames - skipped || state[3] == 65535 ||
            state[2] > state[3]) return AFX_BAD_SAMPLE;
        return AFX_OK;
    }
    return AFX_BAD_SAMPLE;
}
static int relocations_are_sorted(const uint8_t *file, const afx_file_header_t *h) {
    for (uint32_t i = 1; i < h->relocation_count; ++i)
        if (relocation_at(file, h, i - 1).pair_offset >= relocation_at(file, h, i).pair_offset)
            return 0;
    return 1;
}
static int has_relocation(const uint8_t *file, const afx_file_header_t *h, uint32_t pair,
                          int sorted) {
    if (sorted) {
        uint32_t low = 0, high = h->relocation_count;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            uint32_t value = relocation_at(file, h, middle).pair_offset;
            if (value == pair) return 1;
            if (value < pair) low = middle + 1;
            else high = middle;
        }
        return 0;
    }
    for (uint32_t i = 0; i < h->relocation_count; ++i)
        if (relocation_at(file, h, i).pair_offset == pair) return 1;
    return 0;
}
static int is_pair_site(const uint8_t *image, const afx_file_header_t *h, uint32_t pair) {
    if (pair >= h->setups_offset && (pair - h->setups_offset) % AFX_SETUP_BYTES == 0 &&
        (pair - h->setups_offset) / AFX_SETUP_BYTES < h->setup_count) return 1;
    for (uint32_t pc = h->stream_offset; pc < h->stream_offset + h->stream_size;) {
        afx_event_t e;
        if (afx_decode_event(image + pc, h->stream_offset + h->stream_size - pc, &e)) return 0;
        if ((e.opcode == AFX_OP_NOTE || e.opcode == AFX_OP_PATCH) &&
            (e.mask & 3) == 3 && pair == (uint32_t)(e.values - image)) return 1;
        pc += e.bytes;
    }
    return 0;
}

static afx_result_t validate_program(const uint8_t *file, const afx_file_header_t *h,
                                      const uint8_t *image, const afx_sample_t *external) {
    uint16_t states[64][AFX_FIELD_COUNT] = {{0}};
    uint8_t initialized[64] = {0};
    int sorted = file && relocations_are_sorted(file, h);
    for (uint32_t i = 0; i < h->setup_count; ++i) {
        uint32_t offset = h->setups_offset + i * AFX_SETUP_BYTES;
        uint16_t state[AFX_FIELD_COUNT];
        afx_apply_fields(state, image + offset, 0, NULL, 0);
        if (file && !(state[0] & 0x400) && !has_relocation(file, h, offset, sorted)) return AFX_BAD_RELOCATION;
        afx_result_t r = validate_state(file, h, state, external);
        if (r) return r;
    }
    uint32_t elapsed = 0, pc = h->stream_offset, end = pc + h->stream_size;
    int terminal = 0;
    while (pc < end) {
        afx_event_t e;
        afx_result_t r = afx_decode_event(image + pc, end - pc, &e);
        if (r) return r;
        pc += e.bytes;
        if (e.wait > UINT32_MAX - elapsed) return AFX_BAD_BOUNDS;
        elapsed += e.wait;
        if (e.opcode == AFX_OP_END || e.opcode == AFX_OP_PARK) {
            if (pc != end || (e.opcode == AFX_OP_PARK && !(h->flags & AFX_FLAG_CONTROLLED)))
                return AFX_BAD_COMMAND;
            terminal = 1;
        }
        if (e.opcode >= AFX_OP_NOTE && e.opcode <= AFX_OP_KEYOFF) {
            if (e.channel >= h->required_channels) return AFX_BAD_COMMAND;
            if (e.opcode == AFX_OP_NOTE) {
                if (e.setup >= h->setup_count) return AFX_BAD_COMMAND;
                afx_apply_fields(states[e.channel], image + h->setups_offset + e.setup * AFX_SETUP_BYTES,
                                  e.mask, e.values, afx_field_value_bytes(e.mask));
                initialized[e.channel] = 1;
            } else if (e.opcode == AFX_OP_PATCH) {
                if (!initialized[e.channel]) return AFX_BAD_COMMAND;
                afx_apply_fields(states[e.channel], NULL, e.mask, e.values, afx_field_value_bytes(e.mask));
            }
            if (e.opcode != AFX_OP_KEYOFF) {
                /* Exported address updates carry the whole split address. IPC does not have this restriction. */
                if ((e.mask & 3) && (e.mask & 3) != 3) return AFX_BAD_RELOCATION;
                if (!file && (e.mask & 3)) return AFX_BAD_RELOCATION; /* Bind addresses through setups. */
                if (file && (e.mask & 3) && !(states[e.channel][0] & 0x400) &&
                    !has_relocation(file, h, (uint32_t)(e.values - image), sorted)) return AFX_BAD_RELOCATION;
                r = validate_state(file, h, states[e.channel], external);
                if (r) return r;
            }
        }
    }
    if (!terminal) return AFX_BAD_COMMAND;
    return AFX_OK;
}

static uint32_t crc32_metadata(const uint8_t *data, uint32_t size, uint32_t zero_offset) {
    static const uint32_t table[256] = {
+    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
    };
    uint32_t crc = ~0u;
    for (uint32_t i = 0; i < size; ++i) {
        uint8_t byte = i >= zero_offset && i < zero_offset + 4u ? 0 : data[i];
        crc = table[(crc ^ byte) & 0xffu] ^ (crc >> 8);
    }
    return ~crc;
}

static afx_result_t file_validate(const void *data, uint32_t size, afx_file_header_t *out,
                                  int verify_crc) {
    const uint8_t *file = data, *image;
    afx_file_header_t h;
    if (!data || size < sizeof(h)) return AFX_BAD_BOUNDS;
    read_words(&h, file, sizeof(h));
    if (h.magic != AFX_FILE_MAGIC || (h.abi != AFX_ABI_VERSION) ||
        (h.flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_MUSIC | AFX_FLAG_METADATA |
                     AFX_FLAG_MUSIC_CHORUS | AFX_FLAG_LANES))) return AFX_BAD_FORMAT;
    if (h.total_size != size || h.image_offset < sizeof(h) || (h.image_offset & 31) ||
        !afx_range(h.image_offset, h.image_size, size) || h.image_size != size - h.image_offset ||
        !h.image_size || h.image_size > AFX_ASSET_LIMIT || !h.stream_size ||
        !afx_range(h.stream_offset, h.stream_size, h.image_size) ||
        (h.setups_offset & 1) || h.setups_offset > h.image_size ||
        h.setup_count > (h.image_size - h.setups_offset) / AFX_SETUP_BYTES ||
        h.setup_count > 65536 || h.required_channels > 64 || !h.required_channels ||
        !h.tick_rate_num || !h.tick_rate_den ||
        ((h.flags & AFX_FLAG_LANES) &&
         (h.setup_count > (h.image_size - h.setups_offset) / AFX_SETUP_BYTES ||
          h.setups_offset + h.setup_count * AFX_SETUP_BYTES > h.stream_offset ||
          h.required_channels > h.stream_offset -
              (h.setups_offset + h.setup_count * AFX_SETUP_BYTES)))) return AFX_BAD_BOUNDS;
    if (!table(h.samples_offset, h.sample_count, 16, h.image_offset) ||
        !table(h.relocations_offset, h.relocation_count, 12, h.image_offset) ||
        !table(h.checkpoints_offset, h.checkpoints_size, 1, h.image_offset))
        return AFX_BAD_BOUNDS;
    if (overlap(h.samples_offset, h.sample_count * 16, h.relocations_offset, h.relocation_count * 12) ||
        overlap(h.samples_offset, h.sample_count * 16, h.checkpoints_offset, h.checkpoints_size) ||
        overlap(h.relocations_offset, h.relocation_count * 12, h.checkpoints_offset, h.checkpoints_size) ||
        overlap(h.stream_offset, h.stream_size, h.setups_offset, h.setup_count * AFX_SETUP_BYTES))
        return AFX_BAD_BOUNDS;
    if ((h.flags & AFX_FLAG_METADATA) || (h.image_offset >= 96 &&
        afx_read32(file + h.image_offset - 16) == AFX_METADATA_MAGIC)) {
        if (h.image_offset < 96) return AFX_BAD_BOUNDS;
        const uint8_t *footer = file + h.image_offset - 16;
        uint32_t length = afx_read32(footer + 8);
        if (afx_read32(footer) != AFX_METADATA_MAGIC || afx_read32(footer + 4) != AFX_CONTAINER_VERSION)
            return AFX_BAD_FORMAT;
        if (!length || length > 65536) return AFX_BAD_BOUNDS;
        uint32_t block = (length + 47u) & ~31u;
        if (block > h.image_offset - 80) return AFX_BAD_BOUNDS;
        uint32_t start = h.image_offset - block;
        if (h.samples_offset + h.sample_count * 16 > start ||
            h.relocations_offset + h.relocation_count * 12 > start ||
            h.checkpoints_offset + h.checkpoints_size > start) return AFX_BAD_BOUNDS;
        if (verify_crc && crc32_metadata(file, size, h.image_offset - 4) != afx_read32(footer + 12))
            return AFX_BAD_FORMAT;
    }
    image = file + h.image_offset;
    for (uint32_t i = 0; i < h.sample_count; ++i) {
        afx_sample_t s = sample_at(file, &h, i);
        if (!s.frames || !s.byte_size || !afx_range(s.image_offset, s.byte_size, h.image_size) ||
            s.format > AFX_ADPCM ||
            (s.format == AFX_PCM16 && ((s.image_offset & 1) || s.frames > s.byte_size / 2)) ||
            (s.format == AFX_PCM8 && s.frames > s.byte_size) ||
            (s.format == AFX_ADPCM && (s.frames / 2 + (s.frames & 1)) > s.byte_size) ||
            overlap(s.image_offset, s.byte_size, h.stream_offset, h.stream_size) ||
            overlap(s.image_offset, s.byte_size, h.setups_offset, h.setup_count * AFX_SETUP_BYTES) ||
            ((h.flags & AFX_FLAG_LANES) && overlap(s.image_offset, s.byte_size,
                h.setups_offset + h.setup_count * AFX_SETUP_BYTES, h.required_channels)))
            return AFX_BAD_SAMPLE;
        for (uint32_t j = 0; j < i; ++j) {
            afx_sample_t other = sample_at(file, &h, j);
            if (overlap(s.image_offset, s.byte_size, other.image_offset, other.byte_size))
                return AFX_BAD_SAMPLE;
        }
    }
    /* ponytail: load-time quadratic scans keep this validator allocation-free.
     * Index sites/sample intervals if measured compiler output makes loading slow. */
    int relocations_sorted = relocations_are_sorted(file, &h);
    for (uint32_t i = 0; i < h.relocation_count; ++i) {
        afx_relocation_t r = relocation_at(file, &h, i);
        if (r.sample_index >= h.sample_count || !afx_range(r.pair_offset, 4, h.image_size) ||
            !is_pair_site(image, &h, r.pair_offset)) return AFX_BAD_RELOCATION;
        afx_sample_t s = sample_at(file, &h, r.sample_index);
        uint16_t control = afx_read16(image + r.pair_offset);
        uint32_t address = ((uint32_t)(control & 0x7f) << 16) | afx_read16(image + r.pair_offset + 2);
        if (r.byte_offset >= s.byte_size || (s.format == AFX_PCM16 && (r.byte_offset & 1)) ||
            address != s.image_offset + r.byte_offset || (control & 0x400) ||
            ((control >> 7) & 3) != s.format) return AFX_BAD_RELOCATION;
        if (!relocations_sorted)
            for (uint32_t j = 0; j < i; ++j)
                if (relocation_at(file, &h, j).pair_offset == r.pair_offset) return AFX_BAD_RELOCATION;
    }
    if (h.flags & AFX_FLAG_LANES) {
        const uint8_t *lanes = image + h.setups_offset + h.setup_count * AFX_SETUP_BYTES;
        for (uint32_t i = 0; i < h.required_channels; ++i)
            if (lanes[i] >= AFX_MAX_FLOW_CHANNELS) return AFX_BAD_FORMAT;
    }
    afx_result_t result = validate_program(file, &h, image, NULL);
    if (!result && out) *out = h;
    return result;
}

afx_result_t afx_file_validate(const void *data, uint32_t size, afx_file_header_t *out) {
    return file_validate(data, size, out, 0);
}

afx_result_t afx_file_validate_crc(const void *data, uint32_t size, afx_file_header_t *out) {
    return file_validate(data, size, out, 1);
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

/* Host-built SFX images use the same state/stream validator, but their sample
 * ranges are retained external allocations rather than embedded file sections. */
afx_result_t afx_sfx_validate(const void *image, const afx_file_header_t *h,
                              const afx_sample_t *samples) {
    if (!image || !h || (h->sample_count && !samples) ||
        (h->flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_MUSIC | AFX_FLAG_MUSIC_CHORUS |
                      AFX_FLAG_LANES)) || !h->required_channels ||
        h->required_channels > 64 || !h->tick_rate_num || !h->tick_rate_den ||
        !h->image_size || h->image_size > AFX_ASSET_LIMIT || h->setups_offset ||
        h->setup_count > 65536 || h->setup_count > h->image_size / AFX_SETUP_BYTES ||
        h->stream_offset != h->setup_count * AFX_SETUP_BYTES +
            ((h->flags & AFX_FLAG_LANES) ? h->required_channels : 0) || !h->stream_size ||
        !afx_range(h->stream_offset, h->stream_size, h->image_size)) return AFX_BAD_BOUNDS;
    for (uint32_t i = 0; i < h->sample_count; ++i) {
        const afx_sample_t *s = &samples[i];
        if (!s->frames || !s->byte_size || !afx_range(s->image_offset, s->byte_size, AFX_ASSET_LIMIT) ||
            s->format > AFX_ADPCM ||
            (s->format == AFX_PCM16 && ((s->image_offset & 1) || s->frames > s->byte_size / 2)) ||
            (s->format == AFX_PCM8 && s->frames > s->byte_size) ||
            (s->format == AFX_ADPCM && s->frames / 2 + (s->frames & 1) > s->byte_size)) return AFX_BAD_SAMPLE;
    }
    if (h->flags & AFX_FLAG_LANES) {
        const uint8_t *lanes = (const uint8_t *)image + h->setup_count * AFX_SETUP_BYTES;
        for (uint32_t i = 0; i < h->required_channels; ++i)
            if (lanes[i] >= AFX_MAX_FLOW_CHANNELS) return AFX_BAD_FORMAT;
    }
    return validate_program(NULL, h, image, samples);
}

afx_result_t afx_link_validated_image(const void *data, const afx_file_header_t *h, uint32_t base,
                                      void *output, uint32_t capacity) {
    if (!data || !h) return AFX_BAD_BOUNDS;
    if (!output || (base & (AFX_UPLOAD_ALIGN - 1)) || base < AFX_FIRMWARE_INFO_OFFSET + AFX_FIRMWARE_INFO_BYTES ||
        !afx_range(base, h->image_size, AFX_ASSET_LIMIT) || capacity < h->image_size) return AFX_BAD_BOUNDS;
    /* Caller must supply disjoint input/output buffers. Host loader owns arena validation. */
    const uint8_t *file = data;
    uint8_t *image = output;
    memcpy(image, file + h->image_offset, h->image_size);
    for (uint32_t i = 0; i < h->relocation_count; ++i) {
        afx_relocation_t r = relocation_at(file, h, i);
        afx_sample_t s = sample_at(file, h, r.sample_index);
        uint32_t address = base + s.image_offset + r.byte_offset;
        uint16_t control = afx_read16(image + r.pair_offset);
        afx_write16(image + r.pair_offset, (uint16_t)((control & ~0x7fu) | (address >> 16)));
        afx_write16(image + r.pair_offset + 2, (uint16_t)address);
    }
    return AFX_OK;
}

afx_result_t afx_link_validated_image_inplace(void *data, const afx_file_header_t *h,
                                              uint32_t base, uint32_t capacity) {
    if (!data || !h || capacity < h->total_size ||
        (base & (AFX_UPLOAD_ALIGN - 1)) || base < AFX_FIRMWARE_INFO_OFFSET + AFX_FIRMWARE_INFO_BYTES ||
        !afx_range(base, h->image_size, AFX_ASSET_LIMIT)) return AFX_BAD_BOUNDS;
    uint8_t *file = data;
    uint8_t *image = file + h->image_offset;
    for (uint32_t i = 0; i < h->relocation_count; ++i) {
        afx_relocation_t r = relocation_at(file, h, i);
        afx_sample_t sample = sample_at(file, h, r.sample_index);
        uint32_t address = base + sample.image_offset + r.byte_offset;
        uint16_t control = afx_read16(image + r.pair_offset);
        afx_write16(image + r.pair_offset, (uint16_t)((control & ~0x7fu) | (address >> 16)));
        afx_write16(image + r.pair_offset + 2, (uint16_t)address);
    }
    memmove(file, image, h->image_size);
    return AFX_OK;
}

afx_result_t afx_link_image(const void *data, uint32_t size, uint32_t base,
                            void *output, uint32_t capacity, afx_file_header_t *out) {
    afx_file_header_t h;
    afx_result_t result = afx_file_validate(data, size, &h);
    if (result) return result;
    result = afx_link_validated_image(data, &h, base, output, capacity);
    if (result) return result;
    if (out) *out = h;
    return AFX_OK;
}

afx_result_t afx_firmware_validate(const void *data, uint32_t size, afx_firmware_info_t *out) {
    afx_firmware_info_t h;
    if (!data || size < AFX_FIRMWARE_INFO_OFFSET + sizeof(h)) return AFX_BAD_FIRMWARE;
    read_words(&h, (const uint8_t *)data + AFX_FIRMWARE_INFO_OFFSET, sizeof(h));
    if (h.magic != AFX_FIRMWARE_MAGIC || h.abi != AFX_ABI_VERSION || h.layout_id != AFX_LAYOUT_ID ||
        h.load_bytes != size || size > AFX_ASSET_LIMIT || h.asset_base < size ||
        h.asset_base - size >= AFX_UPLOAD_ALIGN || (h.asset_base & (AFX_UPLOAD_ALIGN - 1)) ||
        h.asset_base >= AFX_ASSET_LIMIT || h.asset_limit != AFX_ASSET_LIMIT ||
        h.private_end < AFX_PRIVATE_BASE + 64 * sizeof(afx_runtime_slot_t) ||
        h.private_end > AFX_STACK_BASE || h.stack_base != AFX_STACK_BASE) return AFX_BAD_FIRMWARE;
    if (out) *out = h;
    return AFX_OK;
}
