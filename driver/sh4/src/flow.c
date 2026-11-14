#include "host_internal.h"
#include <aicaflow/bank.h>

#ifndef AFX_VALIDATE_ASSETS
#define AFX_VALIDATE_ASSETS 0
#endif

/* An AFX header, or an AFM's in an AFX header's terms, for its banks. */
int flow_bank_header(const void *data, uint32_t size, afx_file_header_t *out) {
    if (data && size >= 4 && afx_read32(data) == AFX_SONG_MAGIC) {
        afx_song_header_t song;
        afx_result_t valid = afx_song_validate_layout(data, size, &song, NULL);
        if (valid) return -(int)valid;
        *out = (afx_file_header_t){ .flags = song.flags, .bank_id_low = song.bank_id_low,
                                    .bank_id_high = song.bank_id_high, .banks_offset = song.banks_offset };
        return AFX_OK;
    }
    afx_result_t valid = afx_file_validate_layout(data, size, out);
    return valid ? -(int)valid : AFX_OK;
}
int flow_upload(const afx_bank_t *const banks[], uint32_t count, const void *data, uint32_t size,
                uint32_t place, uint32_t place_bytes, afx_asset_t *out) {
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
    /* A song (AFM) uploads as a flow does: its header is read into an AFX
     * header's terms, the streams standing for the stream, and its setups
     * start after the image header. */
    uint32_t setups_at = 0, loop_index = AFX_SONG_NO_LOOP, song_flags = 0;
    if (afx_read32(file) == AFX_SONG_MAGIC) {
        afx_song_header_t song;
        afx_song_image_header_t song_image;
#if AFX_VALIDATE_ASSETS
        valid = afx_song_validate_profile(data, size, &song, &profile);
#else
        valid = afx_song_validate_layout(data, size, &song, NULL);
#endif
        if (!valid) valid = afx_song_validate_layout(data, size, &song, &song_image);
        if (valid) return -(int)valid;
        header = (afx_file_header_t){
            .magic = AFX_FILE_MAGIC, .abi = AFX_FILE_VERSION, .total_size = size, .flags = song.flags,
            .image_offset = song.image_offset, .image_size = song.image_size,
            .stream_offset = song_image.streams_offset, .stream_size = song_image.streams_size,
            .control_id = song.control_id, .setup_count = song.setup_count,
            .bank_id_low = song.bank_id_low, .bank_id_high = song.bank_id_high,
            .relocations_offset = song.relocations_offset, .relocation_count = song.setup_count,
            .banks_offset = song.banks_offset,
            .required_channels = song.voices ? song.voices : song.channels,
            .tick_rate_num = song.tick_rate_num, .tick_rate_den = song.tick_rate_den,
            .work_profile = song.work_profile };
        setups_at = AFX_SONG_IMAGE_HEADER_BYTES;
        loop_index = song_image.loop_index;
        song_flags = AFX_FLAG_SONG | (song.voices ? AFX_FLAG_POOL : 0);
    } else {
        /* An AFX version 7 header names its banks. Seek data is a separate optional
         * SH-4-only AFC file, so the resident image contains no samples or index. */
#if AFX_VALIDATE_ASSETS
        valid = afx_file_validate_profile(data, size, &header, &profile);
#else
        valid = afx_file_validate_layout(data, size, &header);
#endif
        if (valid) return -(int)valid;
    }
#if !AFX_VALIDATE_ASSETS
    if (!header.work_profile) return -AFX_BAD_FORMAT;
    profile.peak_commands = AFX_WORK_PROFILE_COMMANDS(header.work_profile);
    profile.peak_register_writes = AFX_WORK_PROFILE_WRITES(header.work_profile);
#endif
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
        if (pair != setups_at + i * AFX_SETUP_BYTES || !bytes || slot >= count ||
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
    if (place && align_up(header.image_size, AFX_UPLOAD_ALIGN) > place_bytes) {
        result = -AFX_NO_AICA_RAM; goto failed;
    }
    asset = place ? place_asset(place, header.image_size, false) :
                    reserve_asset(header.image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) { result = place ? -AFX_NO_HOST_RAM : -AFX_NO_AICA_RAM; goto failed; }
    uint32_t index;
    (void)resolve_asset(asset, &index);
    g_assets[index].flags = header.flags | song_flags;
    g_assets[index].stream_offset = header.stream_offset;
    g_assets[index].stream_size = header.stream_size;
    g_assets[index].setup_count = header.setup_count;
    g_assets[index].required_channels = header.required_channels;
    g_assets[index].control_id = header.control_id;
    g_assets[index].bank_id_low = header.bank_id_low;
    g_assets[index].bank_id_high = header.bank_id_high;
    g_assets[index].loop_index = loop_index;
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
    if (asset) discard_asset(asset);
    return result;
}
int afx_flow_upload(const afx_bank_t *const banks[], uint32_t count, const void *data,
                    uint32_t size, afx_asset_t *out) {
    if (out) *out = AFX_ASSET_INVALID;
    if (data && size >= 4 && afx_read32(data) == AFX_SONG_MAGIC) return -AFX_BAD_FORMAT;
    return flow_upload(banks, count, data, size, 0, 0, out);
}
int afx_song_load(const afx_bank_t *const banks[], uint32_t count, const void *data,
                  uint32_t size, afx_asset_t *out) {
    if (out) *out = AFX_ASSET_INVALID;
    if (!data || size < AFX_SONG_HEADER_BYTES || afx_read32(data) != AFX_SONG_MAGIC)
        return -AFX_BAD_FORMAT;
    return flow_upload(banks, count, data, size, 0, 0, out);
}
int afx_bank_flow_upload(const afx_bank_t *bank, const void *data, uint32_t size,
                         afx_asset_t *out) {
    return afx_flow_upload(bank ? &bank : NULL, bank ? 1u : 0u, data, size, out);
}
bool afx_asset_loops(afx_asset_t asset, uint32_t *out_loop_position) {
    HOST_GUARD(false);
    uint32_t index;
    if (!resolve_asset(asset, &index) || !g_assets[index].flow ||
        g_assets[index].loop_index == AFX_SONG_NO_LOOP) return false;
    if (out_loop_position) *out_loop_position = g_assets[index].loop_index;
    return true;
}
