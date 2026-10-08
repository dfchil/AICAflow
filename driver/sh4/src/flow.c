#include "host_internal.h"
#include <aicaflow/bank.h>

int afx_bank_flow_upload(const afx_bank_t *bank, const void *data, uint32_t size,
                         afx_asset_t *out) {
    const uint8_t *file = data;
    afx_file_header_t header;
    uint32_t bank_low, bank_high, relocations_at, relocation_count;
    afx_validation_profile_t profile;
    uint32_t bank_index = UINT32_MAX;
    const uint8_t *image;
    afx_asset_t asset = AFX_ASSET_INVALID;
    int result = -AFX_BAD_FORMAT;
    afx_result_t valid;
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!data || size < AFX_FILE_HEADER_BYTES) return -AFX_BAD_BOUNDS;
    /* AFX version 7's fixed header names one AFB. Seek data is a separate optional
     * SH-4-only AFC file, so the resident image contains no samples or index. */
    valid = afx_file_validate_profile(data, size, &header, &profile);
    if (valid) return -(int)valid;
    bank_low = header.bank_id_low;
    bank_high = header.bank_id_high;
    relocations_at = header.relocations_offset;
    relocation_count = header.relocation_count;
    if ((bank_low || bank_high) &&
        (!bank || !bank->asset || bank->id.low != bank_low || bank->id.high != bank_high))
        return -AFX_BAD_SAMPLE;
    if (!(bank_low || bank_high) && (bank || relocation_count)) return -AFX_BAD_SAMPLE;
    if (bank && (!resolve_asset(bank->asset, &bank_index) || !g_assets[bank_index].sample_bank ||
                 g_assets[bank_index].uploading)) return -AFX_INVALID_HANDLE;
    if (bank && (bank->bytes != g_assets[bank_index].size ||
                 (g_assets[bank_index].addr & 31u))) return -AFX_BAD_SAMPLE;
    image = file + header.image_offset;
    for (uint32_t i = 0; i < relocation_count; ++i) {
        uint32_t pair = afx_read32(file + relocations_at + i * 12u);
        uint32_t offset = afx_read32(file + relocations_at + i * 12u + 4u);
        uint32_t bytes = afx_read32(file + relocations_at + i * 12u + 8u);
        uint16_t control;
        if (pair != i * AFX_SETUP_BYTES || !bytes || !bank || offset > bank->bytes ||
            bytes > bank->bytes - offset) goto failed;
        control = afx_read16(image + pair);
        if ((control & 0x400u) || (((uint32_t)(control & 0x7fu) << 16) |
            afx_read16(image + pair + 2u)) != offset) goto failed;
    }
    if (header.work_profile &&
        (AFX_WORK_PROFILE_COMMANDS(header.work_profile) != profile.peak_commands ||
         AFX_WORK_PROFILE_WRITES(header.work_profile) != profile.peak_register_writes)) goto failed;
    asset = reserve_asset(header.image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) { result = -AFX_NO_AICA_RAM; goto failed; }
    uint32_t index;
    (void)resolve_asset(asset, &index);
    g_assets[index].flags = header.flags;
    g_assets[index].stream_offset = header.stream_offset;
    g_assets[index].stream_size = header.stream_size;
    g_assets[index].setup_count = header.setup_count;
    g_assets[index].required_channels = header.required_channels;
    g_assets[index].control_id = header.control_id;
    g_assets[index].bank_id_low = header.bank_id_low;
    g_assets[index].bank_id_high = header.bank_id_high;
    g_assets[index].peak_commands = profile.peak_commands;
    g_assets[index].peak_register_writes = profile.peak_register_writes;
    if (bank) {
        g_assets[index].bank = bank->asset;
        ++g_assets[bank_index].references;
    }
    /* Upload unchanged bytes from the caller; retain no SH-4 image. */
    while (g_assets[index].upload_cursor < header.image_size) {
        uint32_t cursor = g_assets[index].upload_cursor;
        uint32_t bytes = header.image_size - cursor;
        if (bytes > AFX_UPLOAD_STEP_BYTES) bytes = AFX_UPLOAD_STEP_BYTES;
        result = afx_mem_upload(g_assets[index].addr + cursor, image + cursor, bytes);
        if (result) goto failed;
        g_assets[index].upload_cursor += bytes;
    }
    g_assets[index].uploading = false;
    g_assets[index].live = g_assets[index].flow = true;
    *out = asset;
    return AFX_OK;
failed:
    if (asset) (void)afx_asset_free(asset);
    return result;
}
