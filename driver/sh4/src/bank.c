#include "host_internal.h"
#include <aicaflow/bank.h>

#include <kos/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__sh__)
#include <malloc.h>
#endif

#define BANK_DMA_CHUNK 65536u
#define BANK_BUFFER_BYTES (BANK_DMA_CHUNK + AFX_UPLOAD_ALIGN)

typedef struct {
    afx_bank_id_t id; /* Authored identity of the AFB payload. */
    uint32_t data_offset; /* Payload byte offset from the beginning of the AFB file. */
    uint32_t data_size; /* Payload length in bytes. */
    uint32_t total_size; /* Complete AFB file length in bytes. */
} bank_header_t;

typedef struct {
    uint32_t control_id; /* AFX control identity to which these checkpoints belong. */
    afx_bank_id_t bank_id; /* Required sample-bank identity, or zero for a bankless flow. */
    uint32_t data_offset; /* Checkpoint payload byte offset from the AFC file start. */
    uint32_t data_size; /* Checkpoint payload length in bytes. */
    uint32_t total_size; /* Complete AFC file length in bytes. */
} seek_header_t;

static int decode_header(const void *data, uint32_t size, bank_header_t *out) {
    const uint8_t *p = data;
    if (!data || !out || size < AFX_BANK_HEADER_BYTES ||
        afx_read32(p) != AFX_BANK_MAGIC || afx_read32(p + 4) != AFX_BANK_VERSION ||
        (!afx_read32(p + 8) && !afx_read32(p + 12))) return -AFX_BAD_FORMAT;
    bank_header_t h = {
        .id = {afx_read32(p + 8), afx_read32(p + 12)},
        .data_offset = afx_read32(p + 16), .data_size = afx_read32(p + 20),
        .total_size = afx_read32(p + 24)
    };
    if (h.total_size != size || h.data_offset < AFX_BANK_HEADER_BYTES ||
        (h.data_offset & (AFX_UPLOAD_ALIGN - 1u)) || !h.data_size ||
        !afx_range(h.data_offset, h.data_size, size) ||
        h.data_size != size - h.data_offset) return -AFX_BAD_BOUNDS;
    *out = h;
    return AFX_OK;
}

static int decode_seek_header(const void *data, uint32_t size, seek_header_t *out) {
    const uint8_t *p = data;
    if (!data || !out || size < AFX_SEEK_HEADER_BYTES ||
        afx_read32(p) != AFX_SEEK_MAGIC || afx_read32(p + 4) != AFX_SEEK_VERSION ||
        !afx_read32(p + 8)) return -AFX_BAD_FORMAT;
    seek_header_t h = {
        .control_id = afx_read32(p + 8),
        .bank_id = {afx_read32(p + 12), afx_read32(p + 16)},
        .data_offset = afx_read32(p + 20), .data_size = afx_read32(p + 24),
        .total_size = afx_read32(p + 28)
    };
    if (h.total_size != size || h.data_offset != AFX_SEEK_HEADER_BYTES || !h.data_size ||
        !afx_range(h.data_offset, h.data_size, size) || h.data_size != size - h.data_offset)
        return -AFX_BAD_BOUNDS;
    *out = h;
    return AFX_OK;
}

static void *bank_buffer_alloc(void) {
#if defined(__sh__)
    return memalign(AFX_UPLOAD_ALIGN, BANK_BUFFER_BYTES);
#else
    return aligned_alloc(AFX_UPLOAD_ALIGN, BANK_BUFFER_BYTES);
#endif
}

static int stream_file_payload(FILE *file, const bank_header_t *header, afx_asset_t *out) {
    uint8_t *buffer = bank_buffer_alloc();
    afx_asset_t asset = AFX_ASSET_INVALID;
    uint32_t offset = 0;
    int result = -AFX_NO_HOST_RAM;
    if (!buffer || fseek(file, (long)header->data_offset, SEEK_SET)) goto done;
    do {
        result = afx_sample_bank_stream_begin(header->data_size, &asset);
        if (result == -AFX_BUSY) thd_pass();
    } while (result == -AFX_BUSY);
    if (result) goto done;
    while (offset < header->data_size) {
        uint32_t bytes = header->data_size - offset;
        if (bytes > BANK_DMA_CHUNK) bytes = BANK_DMA_CHUNK;
        memset(buffer, 0, BANK_BUFFER_BYTES);
        if (fread(buffer, 1, bytes, file) != bytes) { result = -AFX_BAD_FORMAT; goto done; }
        do {
            result = afx_sample_bank_stream_dma_begin(asset, offset, buffer, bytes);
            if (result == -AFX_BUSY) thd_pass();
        } while (result == -AFX_BUSY);
        if (result) goto done;
        /* The DMA poll distinguishes queued from completed only through its
         * out parameter; wait locally so the stack buffer cannot be reused. */
        for (;;) {
            bool complete = false;
            result = afx_sample_bank_stream_dma_poll(asset, &complete);
            if (result || complete) break;
            thd_pass();
        }
        if (result) goto done;
        offset += bytes;
    }
    result = afx_sample_bank_stream_finish(asset);
    if (!result) *out = asset;
done:
    if (result && asset) (void)afx_asset_free(asset);
    free(buffer);
    return result;
}

int afx_bank_load_memory(afx_bank_t *bank, const void *data, uint32_t size) {
    bank_header_t header;
    if (!bank || bank->asset) return -AFX_BAD_BOUNDS;
    int result = decode_header(data, size, &header);
    if (result) return result;
    afx_asset_t asset = AFX_ASSET_INVALID;
    result = afx_sample_bank_upload((const uint8_t *)data + header.data_offset,
                                    header.data_size, &asset);
    if (result) return result;
    bank->asset = asset;
    bank->id = header.id;
    bank->bytes = header.data_size;
    return AFX_OK;
}

int afx_bank_load_file(afx_bank_t *bank, const char *path) {
    uint8_t raw[AFX_BANK_HEADER_BYTES];
    bank_header_t header;
    FILE *file;
    long size;
    if (!bank || bank->asset || !path || !(file = fopen(path, "rb"))) return -AFX_BAD_BOUNDS;
    int result = -AFX_BAD_FORMAT;
    if (fread(raw, 1, sizeof(raw), file) != sizeof(raw) || fseek(file, 0, SEEK_END) ||
        (size = ftell(file)) < 0 || (uint64_t)size > UINT32_MAX ||
        (uint32_t)size != afx_read32(raw + 24) ||
        (result = decode_header(raw, (uint32_t)size, &header))) goto done;
    result = stream_file_payload(file, &header, &bank->asset);
    if (!result) { bank->id = header.id; bank->bytes = header.data_size; }
done:
    fclose(file);
    return result;
}

int afx_bank_release(afx_bank_t *bank) {
    if (!bank) return -AFX_BAD_BOUNDS;
    if (bank->asset) {
        int result = afx_asset_free(bank->asset);
        if (result) return result;
    }
    *bank = (afx_bank_t){0};
    return AFX_OK;
}

int afx_flow_seek_index_load_memory(afx_asset_t flow, const void *data, uint32_t size) {
    seek_header_t header;
    uint32_t index;
    uint8_t *copy;
    HOST_GUARD(-AFX_BUSY);
    if (!resolve_asset(flow, &index) || !g_assets[index].flow || g_assets[index].references)
        return -AFX_INVALID_HANDLE;
    int result = decode_seek_header(data, size, &header);
    if (result) return result;
    afx_asset_slot_t *slot = &g_assets[index];
    if (header.control_id != slot->control_id ||
        header.bank_id.low != slot->bank_id_low ||
        header.bank_id.high != slot->bank_id_high) return -AFX_BAD_SAMPLE;
    /* Catch a corrupt sidecar at load time; detailed bounds are checked while
     * selecting a checkpoint, where the flow channel count is available. */
    const uint8_t *payload = (const uint8_t *)data + header.data_offset;
    if (header.data_size < 16 || afx_read32(payload) != AFX_CHECKPOINT_MAGIC ||
        afx_read32(payload + 4) != AFX_CHECKPOINT_VERSION || afx_read32(payload + 12))
        return -AFX_BAD_FORMAT;
    copy = malloc(header.data_size);
    if (!copy) return -AFX_NO_HOST_RAM;
    memcpy(copy, payload, header.data_size);
    free(slot->checkpoints);
    slot->checkpoints = copy;
    slot->checkpoints_size = header.data_size;
    return AFX_OK;
}

int afx_flow_seek_index_load_file(afx_asset_t flow, const char *path) {
    FILE *file;
    long bytes;
    uint8_t *data;
    int result;
    if (!path || !(file = fopen(path, "rb"))) return -AFX_BAD_BOUNDS;
    if (fseek(file, 0, SEEK_END) || (bytes = ftell(file)) < 0 ||
        (uint64_t)bytes > UINT32_MAX || fseek(file, 0, SEEK_SET)) {
        fclose(file); return -AFX_BAD_BOUNDS;
    }
    data = malloc((uint32_t)bytes);
    if (!data) { fclose(file); return -AFX_NO_HOST_RAM; }
    if (fread(data, 1, (uint32_t)bytes, file) != (uint32_t)bytes) result = -AFX_BAD_FORMAT;
    else result = afx_flow_seek_index_load_memory(flow, data, (uint32_t)bytes);
    free(data);
    fclose(file);
    return result;
}
