#ifndef AICAFLOW_CODEC_H
#define AICAFLOW_CODEC_H
#include "aicaflow_protocol.h"

/* Wire data may be unaligned. Never cast input bytes to these C structs. */
static inline uint16_t afx_read16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
static inline uint32_t afx_read32(const uint8_t *p) {
    return (uint32_t)afx_read16(p) | (uint32_t)afx_read16(p + 2) << 16;
}
static inline void afx_write16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void afx_write32(uint8_t *p, uint32_t v) {
    afx_write16(p, (uint16_t)v); afx_write16(p + 2, (uint16_t)(v >> 16));
}
static inline int afx_range(uint32_t offset, uint32_t bytes, uint32_t limit) {
    return offset <= limit && bytes <= limit - offset;
}
typedef struct {
    uint8_t opcode, channel;
    uint16_t setup;
    uint32_t wait, mask, bytes;
    const uint8_t *values;
} afx_event_t;

static inline uint32_t afx_field_value_bytes(uint32_t mask) {
    uint32_t bytes = 0;
    for (; mask; mask &= mask - 1u) bytes += 2;
    return bytes;
}
/* Kept inline so host validation and the ARM executor share bytecode bounds,
 * opcode and field-mask interpretation without linking the host-only validator. */
static inline afx_result_t afx_decode_event(const uint8_t *p, uint32_t size,
                                            afx_event_t *out) {
    afx_event_t e = {0};
    uint32_t prefix = 1;
    if (!p || !out || !size) return AFX_BAD_BOUNDS;
    e.opcode = p[0];
    switch (e.opcode) {
    case AFX_OP_END: case AFX_OP_PARK: break;
    case AFX_OP_WAIT8: prefix = 2; break;
    case AFX_OP_WAIT16: prefix = 3; break;
    case AFX_OP_WAIT32: prefix = 5; break;
    case AFX_OP_KEYOFF: prefix = 2; break;
    case AFX_OP_NOTE: prefix = 8; break;
    case AFX_OP_NOTE_PL: prefix = 4; break;
    case AFX_OP_PATCH_LEVEL: prefix = 2; break;
    case AFX_OP_PATCH: prefix = 6; break;
    default: return AFX_BAD_COMMAND;
    }
    if (size < prefix) return AFX_BAD_BOUNDS;
    /* Normalize the compact wire form; all consumers use normal NOTE/PATCH semantics. */
    if (e.opcode == AFX_OP_NOTE_PL) e.opcode = AFX_OP_NOTE;
    if (e.opcode == AFX_OP_PATCH_LEVEL) e.opcode = AFX_OP_PATCH;
    if (e.opcode >= AFX_OP_NOTE && e.opcode <= AFX_OP_KEYOFF) {
        e.channel = p[1];
        if (e.channel >= AFX_MAX_FLOW_CHANNELS) return AFX_BAD_COMMAND;
    }
    if (e.opcode == AFX_OP_NOTE || e.opcode == AFX_OP_PATCH) {
        if (e.opcode == AFX_OP_NOTE) e.setup = afx_read16(p + 2);
        e.mask = p[0] == AFX_OP_NOTE_PL ? AFX_NOTE_PL_MASK :
                 p[0] == AFX_OP_PATCH_LEVEL ? (1u << AFX_FIELD_TOTAL_LEVEL) : afx_read32(p + prefix - 4);
        if (e.mask & ~AFX_FIELD_MASK) return AFX_BAD_COMMAND;
        e.values = p + prefix;
    }
    if (e.opcode == AFX_OP_WAIT8) e.wait = p[1];
    if (e.opcode == AFX_OP_WAIT16) e.wait = afx_read16(p + 1);
    if (e.opcode == AFX_OP_WAIT32) e.wait = afx_read32(p + 1);
    if (e.opcode >= AFX_OP_WAIT8 && e.opcode <= AFX_OP_WAIT32 && !e.wait)
        return AFX_BAD_COMMAND;
    e.bytes = prefix + afx_field_value_bytes(e.mask);
    if (e.bytes > size) return AFX_BAD_BOUNDS;
    *out = e;
    return AFX_OK;
}
afx_result_t afx_encode_event(uint8_t *out, uint32_t capacity, const afx_event_t *event,
                              const uint16_t *values, uint32_t *written);
/* Kept inline so the ARM executor and host validator perform the exact same
 * setup-plus-diff reconstruction without linking the full file validator. */
static inline afx_result_t afx_apply_fields(uint16_t state[AFX_FIELD_COUNT],
                                            const uint8_t *setup, uint32_t mask,
                                            const uint8_t *values, uint32_t bytes) {
    uint16_t result[AFX_FIELD_COUNT];
    uint32_t expected = 0;
    if (!state || (mask & ~AFX_FIELD_MASK)) return AFX_BAD_COMMAND;
    for (uint32_t scan = mask; scan; scan &= scan - 1u) expected += 2;
    if (bytes != expected || (bytes && !values)) return AFX_BAD_BOUNDS;
    for (uint32_t i = 0; i < AFX_FIELD_COUNT; ++i) {
        if (setup) result[i] = afx_read16(setup + 2 * i);
        else result[i] = state[i];
        if (mask & (1u << i)) { result[i] = afx_read16(values); values += 2; }
    }
    for (uint32_t i = 0; i < AFX_FIELD_COUNT; ++i) state[i] = result[i];
    return AFX_OK;
}
/* NOTE always has a complete immutable setup. This avoids requiring a caller to
 * initialize a scratch register image merely to reconstruct that complete state. */
static inline afx_result_t afx_apply_setup_fields(uint16_t state[AFX_FIELD_COUNT],
                                                  const uint8_t *setup, uint32_t mask,
                                                  const uint8_t *values, uint32_t bytes) {
    uint32_t expected = afx_field_value_bytes(mask);
    if (!state || !setup || (mask & ~AFX_FIELD_MASK)) return AFX_BAD_COMMAND;
    if (bytes != expected || (bytes && !values)) return AFX_BAD_BOUNDS;
    for (uint32_t i = 0; i < AFX_FIELD_COUNT; ++i) state[i] = afx_read16(setup + 2 * i);
    for (uint32_t i = 0; i < AFX_FIELD_COUNT; ++i)
        if (mask & (1u << i)) { state[i] = afx_read16(values); values += 2; }
    return AFX_OK;
}
/* Host SFX image validation; sample image_offset fields hold resolved AICA
 * addresses. The host must separately prove ownership of those allocations. */
afx_result_t afx_sfx_validate(const void *image, const afx_file_header_t *header,
                              const afx_sample_t *samples);
/* Runtime validation checks structure only. Use this explicitly for offline
 * integrity checking of metadata-bearing containers. */
afx_result_t afx_file_validate(const void *data, uint32_t size, afx_file_header_t *out);
afx_result_t afx_file_validate_crc(const void *data, uint32_t size, afx_file_header_t *out);
/* Validates an AFX file and totals its WAIT instructions. Outputs are written
 * only on success. */
afx_result_t afx_flow_duration(const void *data, uint32_t size, uint64_t *out_ticks,
                               uint32_t *out_tick_rate_num, uint32_t *out_tick_rate_den);
/* Input and output buffers must be disjoint. Validation failures leave output
 * unchanged. The loader must additionally prove image_base belongs to its allocation. */
afx_result_t afx_link_image(const void *data, uint32_t size, uint32_t image_base,
                            void *image, uint32_t capacity, afx_file_header_t *out);
/* Fast path for a header already returned by afx_file_validate(). */
afx_result_t afx_link_validated_image(const void *data, const afx_file_header_t *header,
                                      uint32_t image_base, void *image, uint32_t capacity);
/* Links a mutable AFX container then compacts its image to data[0]. The caller
 * retains data until the associated flow releases its host image. */
afx_result_t afx_link_validated_image_inplace(void *data, const afx_file_header_t *header,
                                              uint32_t image_base, uint32_t capacity);
afx_result_t afx_firmware_validate(const void *data, uint32_t size, afx_firmware_info_t *out);
void afx_encode_header(uint8_t out[AFX_FILE_HEADER_BYTES], const afx_file_header_t *header);
#endif
