#include "host_internal.h"
#include <aicaflow/bank.h>

static afx_result_t profile_stream(const afx_file_header_t *header, const uint8_t *image,
                                   uint32_t *peak_commands, uint32_t *peak_writes) {
    uint32_t cursor = header->stream_offset, end = cursor + header->stream_size;
    uint32_t commands = 0, writes = 0, max_commands = 0, max_writes = 0;
    while (cursor < end) {
        afx_event_t event;
        afx_result_t result = afx_decode_event(image + cursor, end - cursor, &event);
        if (result) return result;
        cursor += event.bytes;
        if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) {
            if (commands > max_commands) max_commands = commands;
            if (writes > max_writes) max_writes = writes;
            commands = writes = 0;
        } else if (event.opcode == AFX_OP_NOTE) {
            ++commands; writes += 19;
        } else if (event.opcode == AFX_OP_PATCH) {
            ++commands; writes += afx_field_value_bytes(event.mask) / 2u;
        } else if (event.opcode == AFX_OP_KEYOFF) {
            ++commands; ++writes;
        }
    }
    if (commands > max_commands) max_commands = commands;
    if (writes > max_writes) max_writes = writes;
    *peak_commands = max_commands; *peak_writes = max_writes;
    return AFX_OK;
}

static int flow_upload_step(afx_asset_t asset, uint32_t max_bytes,
                         uint32_t *out_uploaded, bool *out_complete) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (out_uploaded) *out_uploaded = 0;
    if (out_complete) *out_complete = false;
    if (!max_bytes || !resolve_asset(asset, &index) || !g_assets[index].uploading)
        return -AFX_BAD_BOUNDS;
    afx_asset_slot_t *slot = &g_assets[index];
    uint32_t uploaded = slot->upload_cursor;
    uint32_t remaining = slot->size - uploaded;
    uint32_t amount = max_bytes > AFX_UPLOAD_STEP_BYTES ? AFX_UPLOAD_STEP_BYTES : max_bytes;
    if (amount > remaining) amount = remaining;
    if (amount < remaining) amount &= ~3u;
    if (!amount || afx_mem_upload(slot->addr + uploaded, slot->image + uploaded, amount))
        return -AFX_BAD_BOUNDS;
    slot->upload_cursor += amount;
    if (out_uploaded) *out_uploaded = amount;
    if (slot->upload_cursor == slot->size) {
        slot->uploading = false;
        slot->live = true;
        slot->flow = true;
        if (out_complete) *out_complete = true;
    }
    return AFX_OK;
}

static int finish_flow_upload(afx_asset_t asset, afx_asset_t *out) {
    bool complete = false;
    while (!complete) {
        int result = flow_upload_step(asset, AFX_UPLOAD_STEP_BYTES, NULL, &complete);
        if (result) { (void)afx_asset_free(asset); return result; }
    }
    *out = asset;
    return AFX_OK;
}

int afx_bank_flow_upload(const afx_bank_t *bank, const void *data, uint32_t size,
                         afx_asset_t *out) {
    const uint8_t *file = data;
    afx_file_header_t header;
    uint32_t bank_low, bank_high, relocations_at, relocation_count;
    uint32_t peak_commands, peak_writes;
    uint32_t bank_index = UINT32_MAX;
    uint8_t *image = NULL;
    afx_asset_t asset = AFX_ASSET_INVALID;
    int result = -AFX_BAD_FORMAT;
    afx_result_t valid;
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!data || size < AFX_FILE_HEADER_BYTES) return -AFX_BAD_BOUNDS;
    /* ABI-7's fixed header names one AFB.  Seek data is a separate optional
     * SH-4-only AFC file, so the resident image contains no samples or index. */
    valid = afx_file_validate(data, size, &header);
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
    image = upload_image_alloc(header.image_size);
    if (!image) return -AFX_NO_HOST_RAM;
    memcpy(image, file + header.image_offset, header.image_size);
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
        uint32_t address = afx_asset_addr(bank->asset) + offset;
        afx_write16(image + pair, (control & ~0x7fu) | (address >> 16));
        afx_write16(image + pair + 2u, address);
    }
    if (profile_stream(&header, image, &peak_commands, &peak_writes))
        goto failed;
    if (header.work_profile &&
        (AFX_WORK_PROFILE_COMMANDS(header.work_profile) != peak_commands ||
         AFX_WORK_PROFILE_WRITES(header.work_profile) != peak_writes)) goto failed;
    asset = reserve_asset(header.image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) { result = -AFX_NO_AICA_RAM; goto failed; }
    uint32_t index;
    (void)resolve_asset(asset, &index);
    g_assets[index].header = header;
    g_assets[index].image = image;
    g_assets[index].image_owned = true;
    g_assets[index].peak_commands = peak_commands;
    g_assets[index].peak_register_writes = peak_writes;
    if (bank) {
        g_assets[index].dependencies = malloc(sizeof(*g_assets[index].dependencies));
        if (!g_assets[index].dependencies) { result = -AFX_NO_HOST_RAM; goto failed; }
        g_assets[index].dependencies[0] = bank->asset;
        g_assets[index].dependency_count = 1;
        ++g_assets[bank_index].references;
    }
    result = finish_flow_upload(asset, out);
    if (!result) return AFX_OK;
failed:
    if (asset) (void)afx_asset_free(asset);
    else free(image);
    return result;
}
