#include <aicaflow/firmware.h>
#include <aicaflow/codec.h>
#include <string.h>

/* Structs describe layout, but explicit loads also work on unaligned inputs.
 * memcpy each word into the object avoids type-punning/struct-as-array UB. */
static void read_words(void *out, const uint8_t *p, uint32_t size) {
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t word = afx_read32(p + i);
        memcpy((uint8_t *)out + i, &word, 4);
    }
}
afx_result_t afx_firmware_validate(const void *data, uint32_t size, afx_firmware_info_t *out) {
    afx_firmware_info_t h;
    if (!data || size < AFX_FIRMWARE_INFO_OFFSET + sizeof(h)) return AFX_BAD_FIRMWARE;
    read_words(&h, (const uint8_t *)data + AFX_FIRMWARE_INFO_OFFSET, sizeof(h));
    if (h.magic != AFX_FIRMWARE_MAGIC || h.abi != AFX_ABI_VERSION || h.layout_id != AFX_LAYOUT_ID ||
        h.load_bytes != size || size > AFX_ASSET_MAX || h.asset_base < size ||
        h.asset_base - size >= AFX_UPLOAD_ALIGN || (h.asset_base & (AFX_UPLOAD_ALIGN - 1)) ||
        h.asset_base >= AFX_ASSET_MAX || h.asset_limit != AFX_ASSET_MAX ||
        h.private_end < AFX_PRIVATE_BASE + 64 * sizeof(afx_runtime_slot_t) ||
        h.private_end > AFX_STACK_BASE || h.stack_base != AFX_STACK_BASE) return AFX_BAD_FIRMWARE;
    if (out) *out = h;
    return AFX_OK;
}
