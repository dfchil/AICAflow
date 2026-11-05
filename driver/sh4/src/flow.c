#include "host_internal.h"
#include <aicaflow/bank.h>

#ifndef AFX_VALIDATE_ASSETS
#define AFX_VALIDATE_ASSETS 0
#endif

int afx_flow_upload(const afx_bank_t *const banks[], uint32_t count, const void *data,
                    uint32_t size, afx_asset_t *out) {
    const uint8_t *file = data;
    afx_file_header_t header;
    uint32_t relocations_at, relocation_count;
    afx_validation_profile_t profile;
    uint32_t bank_index[AFX_MAX_FLOW_BANKS];
    const uint8_t *image;
    afx_asset_t asset = AFX_ASSET_INVALID;
    int result = -AFX_BAD_FORMAT;
    afx_result_t valid;
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!data || size < AFX_FILE_HEADER_BYTES) return -AFX_BAD_BOUNDS;
    /* An AFX version 7 header names its banks. Seek data is a separate optional
     * SH-4-only AFC file, so the resident image contains no samples or index. */
#if AFX_VALIDATE_ASSETS
    valid = afx_file_validate_profile(data, size, &header, &profile);
#else
    valid = afx_file_validate_layout(data, size, &header);
    if (!valid) {
        if (!header.work_profile) return -AFX_BAD_FORMAT;
        profile.peak_commands = AFX_WORK_PROFILE_COMMANDS(header.work_profile);
        profile.peak_register_writes = AFX_WORK_PROFILE_WRITES(header.work_profile);
    }
#endif
    if (valid) return -(int)valid;
    relocations_at = header.relocations_offset;
    relocation_count = header.relocation_count;
    /* A zero bank identity is valid only for a no-sample flow, with no banks. */
    if (count != afx_file_bank_count(data, &header) || (count && !banks)) return -AFX_BAD_SAMPLE;
    for (uint32_t slot = 0; slot < count; ++slot) {
        const afx_bank_t *bank = banks[slot];
        uint32_t low, high;
        afx_file_bank_identity(data, &header, slot, &low, &high);
        if (!bank || !bank->asset || bank->id.low != low || bank->id.high != high)
            return -AFX_BAD_SAMPLE;
        if (!resolve_asset(bank->asset, &bank_index[slot]) ||
            !g_assets[bank_index[slot]].sample_bank || g_assets[bank_index[slot]].uploading ||
            g_assets[bank_index[slot]].release_wanted) return -AFX_INVALID_HANDLE;
        if (bank->bytes != g_assets[bank_index[slot]].size ||
            (g_assets[bank_index[slot]].addr & 31u)) return -AFX_BAD_SAMPLE;
    }
    image = file + header.image_offset;
    for (uint32_t i = 0; i < relocation_count; ++i) {
        uint32_t pair = afx_read32(file + relocations_at + i * 12u);
        uint32_t address = afx_read32(file + relocations_at + i * 12u + 4u);
        uint32_t bytes = afx_read32(file + relocations_at + i * 12u + 8u);
        uint32_t slot = (header.flags & AFX_FLAG_BANKS) ? address >> AFX_BANK_SLOT_SHIFT : 0;
        uint32_t offset = (header.flags & AFX_FLAG_BANKS) ? address & AFX_BANK_SLOT_OFFSET_MASK : address;
        uint16_t control;
        if (pair != i * AFX_SETUP_BYTES || !bytes || slot >= count ||
            offset > banks[slot]->bytes || bytes > banks[slot]->bytes - offset) goto failed;
        control = afx_read16(image + pair);
        if ((control & 0x400u) || (((uint32_t)(control & 0x7fu) << 16) |
            afx_read16(image + pair + 2u)) != address) goto failed;
    }
#if AFX_VALIDATE_ASSETS
    if (header.work_profile &&
        (AFX_WORK_PROFILE_COMMANDS(header.work_profile) != profile.peak_commands ||
         AFX_WORK_PROFILE_WRITES(header.work_profile) != profile.peak_register_writes)) goto failed;
#endif
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
    g_assets[index].loop_tick = header.loop_tick;
    g_assets[index].peak_commands = profile.peak_commands;
    g_assets[index].peak_register_writes = profile.peak_register_writes;
    /* The ARM7 finds the banks of slots 1 and up in the table that ends the image. */
    uint32_t table[AFX_MAX_FLOW_BANKS] = {0};
    if (header.flags & AFX_FLAG_BANKS) {
        afx_asset_t extra[AFX_MAX_FLOW_BANKS - 1u] = {0};
        for (uint32_t slot = 1; slot < count; ++slot) {
            extra[slot - 1u] = banks[slot]->asset;
            table[slot] = AFX_BANK_TABLE_ENTRY(g_assets[bank_index[slot]].addr, banks[slot]->bytes);
        }
        if (!flow_banks_add(index, extra)) { result = -AFX_NO_HOST_RAM; goto failed; }
    }
    if (count) g_assets[index].bank = banks[0]->asset;
    for (uint32_t slot = 0; slot < count; ++slot) ++g_assets[bank_index[slot]].references;
    /* Upload unchanged bytes from the caller; retain no SH-4 image. */
    result = afx_mem_upload(g_assets[index].addr, image, header.image_size);
    if (result) goto failed;
    if (header.flags & AFX_FLAG_BANKS)
        upload_words(g_assets[index].addr +
                     AFX_BANK_TABLE_OFFSET(header.stream_offset + header.stream_size),
                     table, sizeof(table));
    g_assets[index].upload_cursor = header.image_size;
    g_assets[index].uploading = false;
    g_assets[index].live = g_assets[index].flow = true;
    *out = asset;
    return AFX_OK;
failed:
    if (asset) (void)afx_asset_free(asset);
    return result;
}
int afx_bank_flow_upload(const afx_bank_t *bank, const void *data, uint32_t size,
                         afx_asset_t *out) {
    return afx_flow_upload(bank ? &bank : NULL, bank ? 1u : 0u, data, size, out);
}
bool afx_asset_loops(afx_asset_t asset, uint32_t *out_loop_tick) {
    HOST_GUARD(false);
    uint32_t index;
    if (!resolve_asset(asset, &index) || !g_assets[index].flow ||
        !(g_assets[index].flags & AFX_FLAG_LOOP)) return false;
    if (out_loop_tick) *out_loop_tick = g_assets[index].loop_tick;
    return true;
}
