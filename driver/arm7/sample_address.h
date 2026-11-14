#ifndef AICAFLOW_SAMPLE_ADDRESS_H
#define AICAFLOW_SAMPLE_ADDRESS_H
#include <aicaflow/protocol.h>
#include "aica_access.h"

enum {
    AFX_SAMPLE_NOISE_ENABLE = 0x400u,
    AFX_SAMPLE_ADDRESS_HIGH_MASK = 0x7fu,
    AFX_SAMPLE_ADDRESS_LOW_BITS = 16u,
    AFX_BANK_ADDRESS_UNIT_SHIFT = 5u /* Bank bases are stored in 32-byte units. */
};

/* A slot's bank table entry; the host test reads it from a buffer instead.
 * The table follows a flow's stream, which ends at context->end, and a song's
 * last stream: context->end there is only the end of the section playing. */
#ifndef AFX_BANK_TABLE_READ
static uint32_t bank_table_address(const volatile afx_runtime_slot_t *context) {
    if (!(context->flags & AFX_FLAG_SONG)) return AFX_BANK_TABLE_OFFSET(context->end);
    const volatile afx_song_image_header_t *image =
        (const volatile afx_song_image_header_t *)AICA_RAM(context->image_base);
    return context->image_base + AFX_BANK_TABLE_OFFSET(image->streams_offset + image->streams_size);
}
#define AFX_BANK_TABLE_READ(context, slot) \
    (*(volatile const uint32_t *)AICA_RAM(bank_table_address(context) + (slot) * 4u))
#endif

/* Convert only the sample address bits; preserve format, loop and key flags.
 * Noise does not fetch bank memory. Call before writing any channel registers.
 * With AFX_FLAG_BANKS, the top two address bits select the bank slot. One
 * copy, not inlined into each voice path, keeps the firmware image small. */
static __attribute__((noinline))
afx_result_t resolve_sample_address(const volatile afx_runtime_slot_t *context,
                                    uint16_t *control, uint16_t *low) {
    if (*control & AFX_SAMPLE_NOISE_ENABLE) return AFX_OK;
    uint32_t base = (uint32_t)context->bank_base_units << AFX_BANK_ADDRESS_UNIT_SHIFT;
    uint32_t end = context->bank_end;
    uint32_t offset = ((uint32_t)(*control & AFX_SAMPLE_ADDRESS_HIGH_MASK) <<
                       AFX_SAMPLE_ADDRESS_LOW_BITS) | *low;
    uint32_t slot = offset >> AFX_BANK_SLOT_SHIFT;
    if ((context->flags & AFX_FLAG_BANKS) && slot) {
        uint32_t entry = AFX_BANK_TABLE_READ(context, slot);
        base = (entry & 0xffffu) << AFX_BANK_ADDRESS_UNIT_SHIFT;
        end = base + ((entry >> 16) << AFX_BANK_ADDRESS_UNIT_SHIFT);
        offset &= AFX_BANK_SLOT_OFFSET_MASK;
    }
    if (base >= end || offset >= end - base)
        return AFX_BAD_SAMPLE;
    uint32_t address = base + offset;
    *control = (*control & ~AFX_SAMPLE_ADDRESS_HIGH_MASK) |
               (address >> AFX_SAMPLE_ADDRESS_LOW_BITS);
    *low = (uint16_t)address;
    return AFX_OK;
}
#endif
