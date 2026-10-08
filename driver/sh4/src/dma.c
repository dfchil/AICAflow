#include "host_internal.h"

void upload_dma_complete(void *ignored) {
    (void)ignored;
    g_upload_dma_done = true;
}
void upload_dma_wait(void) {
    while (g_upload_dma_asset && !g_upload_dma_done) thd_pass();
    /* DMA completion releases the channel; drain the G2 FIFO before another
     * G2 client such as the Broadband Adapter starts a file transaction. */
    if (g_upload_dma_asset) g2_fifo_wait();
    g_upload_dma_asset = AFX_ASSET_INVALID;
    g_upload_dma_bytes = 0;
    g_upload_dma_offset = 0;
    g_upload_dma_stream = false;
    g_upload_dma_done = false;
}
void *upload_image_alloc(uint32_t size) {
    if (!size || size > UINT32_MAX - (AFX_UPLOAD_ALIGN - 1u)) return NULL;
    uint32_t aligned = (size + AFX_UPLOAD_ALIGN - 1u) & ~(AFX_UPLOAD_ALIGN - 1u);
#if defined(__sh__)
    void *image = memalign(AFX_UPLOAD_ALIGN, aligned);
#else
    void *image = aligned_alloc(AFX_UPLOAD_ALIGN, aligned);
#endif
    if (image) memset(image, 0, aligned);
    return image;
}

uint32_t read_spu_word(uint32_t address) {
    return g2_read_32(g_spu_base + address);
}
/* spu_memload rounds inputs to words. A private aligned bounce prevents it from
 * reading after a short, unaligned caller buffer. */
void upload_words(uint32_t address, const void *data, uint32_t size) {
    const uint8_t *source = data;
    /* Linked AFX images and firmware are word-aligned.  Let KOS transfer the
     * aligned body in one call; preserving this bounce path keeps the public
     * byte-oriented upload API safe for unaligned callers and short tails. */
    if (!((uintptr_t)source & 3u)) {
        uint32_t whole = size & ~3u;
        if (whole) {
            spu_memload(address, source, whole);
            address += whole;
            source += whole;
            size -= whole;
        }
    }
    while (size) {
        uint32_t words[8] = {0};
        uint32_t count = size < sizeof(words) ? size : sizeof(words);
        memcpy(words, source, count);
        spu_memload(address, words, (count + 3u) & ~3u);
        address += count;
        source += count;
        size -= count;
    }
}
static int upload_sample_data(const void *data, uint32_t bytes, afx_asset_t *out) {
    if (g_upload_dma_asset) return -AFX_BUSY;
    /* Aligned input goes straight to DMA; only an incomplete final cache line
     * needs staging. Unaligned inputs retain the bounded bounce path. */
    uint32_t capacity = bytes < 65536u ? bytes : 65536u;
    bool direct = !((uintptr_t)data & (AFX_UPLOAD_ALIGN - 1u));
    uint32_t staging_bytes = direct ? (bytes & (AFX_UPLOAD_ALIGN - 1u) ? AFX_UPLOAD_ALIGN : 0) : capacity;
    void *staging = staging_bytes ? upload_image_alloc(staging_bytes) : NULL;
    if (staging_bytes && !staging) return -AFX_NO_HOST_RAM;
    afx_asset_t asset = AFX_ASSET_INVALID;
    int result = afx_sample_bank_stream_begin(bytes, &asset);
    for (uint32_t offset = 0; !result && offset < bytes;) {
        uint32_t count = bytes - offset;
        if (count > capacity) count = capacity;
        const void *source = (const uint8_t *)data + offset;
        if (direct) {
            uint32_t whole = count & ~(AFX_UPLOAD_ALIGN - 1u);
            if (whole) count = whole;
        }
        if (!direct || count < AFX_UPLOAD_ALIGN) {
            memcpy(staging, source, count);
            memset((uint8_t *)staging + count, 0, align_up(count, AFX_UPLOAD_ALIGN) - count);
            source = staging;
        }
        result = afx_sample_bank_stream_dma_begin(asset, offset, source, count);
        bool complete = false;
        while (!result && !complete) {
            result = afx_sample_bank_stream_dma_poll(asset, &complete);
            if (!result && !complete) thd_pass();
        }
        offset += count;
    }
    if (!result) result = afx_sample_bank_stream_finish(asset);
    if (result && asset) (void)afx_asset_free(asset);
    free(staging);
    if (!result) *out = asset;
    return result;
}


int afx_sample_bank_upload(const void *data, uint32_t bytes, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!data || !bytes || bytes > g_asset_limit) return -AFX_BAD_BOUNDS;
    int result = upload_sample_data(data, bytes, out);
    if (!result) {
        uint32_t index;
        (void)resolve_asset(*out, &index);
        g_assets[index].sample_bank = true;
    }
    return result;
}


int afx_sample_bank_stream_begin(uint32_t bytes, afx_asset_t *out) {
    uint32_t index;
    afx_asset_t asset;
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!bytes || bytes > g_asset_limit) return -AFX_BAD_BOUNDS;
    asset = reserve_asset(bytes, AFX_UPLOAD_ALIGN, false);
    if (!asset) return -AFX_NO_AICA_RAM;
    (void)resolve_asset(asset, &index);
    g_assets[index].sample_bank = true;
    *out = asset;
    return AFX_OK;
}

int afx_sample_bank_stream_dma_begin(afx_asset_t asset, uint32_t byte_offset,
                                     const void *data, uint32_t bytes) {
    uint32_t index, padded;
    HOST_GUARD(-AFX_BUSY);
    if (!data || !bytes || ((uintptr_t)data & (AFX_UPLOAD_ALIGN - 1u)) ||
        !resolve_asset(asset, &index) ||
        !g_assets[index].sample_bank || !g_assets[index].uploading)
        return -AFX_BAD_BOUNDS;
    afx_asset_slot_t *bank = &g_assets[index];
    padded = align_up(bytes, AFX_UPLOAD_ALIGN);
    if (!padded || byte_offset != bank->upload_cursor || bytes > bank->size - byte_offset ||
        padded > bank->allocation_size - byte_offset) return -AFX_BAD_BOUNDS;
    if (g_upload_dma_asset) return -AFX_BUSY;
    dcache_wback_range((uintptr_t)data, padded);
    g_upload_dma_asset = asset;
    g_upload_dma_bytes = bytes;
    g_upload_dma_offset = byte_offset;
    g_upload_dma_stream = true;
    g_upload_dma_done = false;
    if (spu_dma_transfer((void *)data, bank->addr + byte_offset, padded, 0,
                         upload_dma_complete, NULL)) {
        g_upload_dma_asset = AFX_ASSET_INVALID;
        g_upload_dma_bytes = g_upload_dma_offset = 0;
        g_upload_dma_stream = false;
        return -AFX_BUSY;
    }
    return AFX_OK;
}

int afx_sample_bank_stream_dma_poll(afx_asset_t asset, bool *out_complete) {
    uint32_t index, bytes;
    HOST_GUARD(-AFX_BUSY);
    if (out_complete) *out_complete = false;
    if (!resolve_asset(asset, &index) || !g_assets[index].sample_bank ||
        !g_assets[index].uploading || g_upload_dma_asset != asset || !g_upload_dma_stream)
        return -AFX_BAD_BOUNDS;
    if (!g_upload_dma_done) return AFX_OK;
    afx_asset_slot_t *bank = &g_assets[index];
    if (bank->upload_cursor != g_upload_dma_offset) return -AFX_BAD_BOUNDS;
    bytes = g_upload_dma_bytes;
    upload_dma_wait();
    bank->upload_cursor += bytes;
    if (out_complete) *out_complete = true;
    return AFX_OK;
}

int afx_sample_bank_stream_finish(afx_asset_t asset) {
    uint32_t index;
    HOST_GUARD(-AFX_BUSY);
    if (!resolve_asset(asset, &index) || !g_assets[index].sample_bank ||
        !g_assets[index].uploading ||
        g_assets[index].upload_cursor != g_assets[index].size)
        return -AFX_BAD_BOUNDS;
    g_assets[index].uploading = false;
    g_assets[index].live = true;
    return AFX_OK;
}
