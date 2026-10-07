#ifndef AICAFLOW_SAMPLE_ADDRESS_H
#define AICAFLOW_SAMPLE_ADDRESS_H
#include <aicaflow/protocol.h>

enum {
    AFX_SAMPLE_NOISE_ENABLE = 0x400u,
    AFX_SAMPLE_ADDRESS_HIGH_MASK = 0x7fu,
    AFX_SAMPLE_ADDRESS_LOW_BITS = 16u,
    AFX_BANK_ADDRESS_UNIT_SHIFT = 5u /* Bank bases are stored in 32-byte units. */
};

/* Convert only the sample address bits; preserve format, loop and key flags.
 * Noise does not fetch bank memory. Call before writing any channel registers. */
static inline afx_result_t resolve_sample_address(const volatile afx_runtime_slot_t *context,
                                                  uint16_t *control, uint16_t *low) {
    if (*control & AFX_SAMPLE_NOISE_ENABLE) return AFX_OK;
    uint32_t base = (uint32_t)context->bank_base_units << AFX_BANK_ADDRESS_UNIT_SHIFT;
    uint32_t offset = ((uint32_t)(*control & AFX_SAMPLE_ADDRESS_HIGH_MASK) <<
                       AFX_SAMPLE_ADDRESS_LOW_BITS) | *low;
    if (base >= context->bank_end || offset >= context->bank_end - base)
        return AFX_BAD_SAMPLE;
    uint32_t address = base + offset;
    *control = (*control & ~AFX_SAMPLE_ADDRESS_HIGH_MASK) |
               (address >> AFX_SAMPLE_ADDRESS_LOW_BITS);
    *low = (uint16_t)address;
    return AFX_OK;
}
#endif
