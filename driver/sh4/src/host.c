#include <aicaflow/host.h>
#include <aicaflow_codec.h>

#include <dc/g2bus.h>
#include <dc/spu.h>
#include <kos/cache.h>
#include <kos/mutex.h>
#include <kos/timer.h>
#include <kos/thread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__sh__)
#include <malloc.h>
#endif

#ifndef AFX_SPU_RAM_BASE_SH4
#define AFX_SPU_RAM_BASE_SH4 0xa0800000u
#endif

typedef struct { uint32_t addr, size; } afx_block_t;
typedef struct {
    bool live, retired, flow, uploading, sample, sfx, image_owned, owns_allocation, sample_bank;
    uint16_t generation;
    uint32_t addr, size, allocation_size, references, upload_cursor;
    uint32_t sample_frames, sample_format, dependency_count;
    afx_asset_t backing;
    afx_asset_t *dependencies;
    uint8_t *checkpoints;
    uint32_t checkpoints_size;
    uint8_t *image;
    uint32_t peak_commands, peak_register_writes;
    afx_file_header_t header; /* Host-only decoded metadata. */
} afx_asset_slot_t;
typedef struct {
    bool live, retired, pending, recycling, work_reserved;
    uint16_t generation;
    uint32_t asset_index, map_addr, staging_addr, start_tick;
    uint8_t required_channels, map_arena, map_offset;
    uint64_t channel_mask;
    afx_instance_status_t status; /* Last stable ARM-owned observation. */
} afx_instance_slot_t;

static uint32_t g_dynamic_base;
static afx_block_t *g_free_blocks, *g_allocs;
static uint32_t g_free_count, g_free_capacity, g_alloc_count, g_alloc_capacity;
static afx_asset_slot_t *g_assets;
static uint32_t g_asset_capacity;
static afx_instance_slot_t g_instances[AFX_MAX_FLOW_SLOTS];
static uint64_t g_available_channels = UINT64_MAX;
static uint64_t g_map_used[AFX_CHANNEL_MAP_ARENAS];
static uint32_t g_reserved_peak_commands, g_reserved_peak_writes;
static bool g_dsp_scene;
static uint16_t g_dsp_return_left, g_dsp_return_right;
static uint32_t g_next_sequence = 1;
static bool g_ready, g_lifecycle;
/* AICA exposes one DMA channel. The interrupt only flips this flag: all asset
 * bookkeeping stays on the calling SH-4 thread, where the host lock applies. */
static volatile bool g_upload_dma_done;
static afx_asset_t g_upload_dma_asset;
static uint32_t g_upload_dma_bytes;
static uint32_t g_upload_dma_offset;
static bool g_upload_dma_stream;
/* ponytail: one host lock; split ownership only if measured contention matters.
 * Recursive because the public upload/seek helpers call public allocation APIs.
 * DSP program installation waits for its one ARM preparation acknowledgement;
 * other gameplay calls return so the caller can retry. */
static mutex_t g_host_mutex = RECURSIVE_MUTEX_INITIALIZER;
static void host_unlock(mutex_t **guard) { if (*guard) mutex_unlock(*guard); }
#define HOST_GUARD(failure) \
    mutex_t *host_guard __attribute__((cleanup(host_unlock))) = NULL; \
    if (mutex_trylock(&g_host_mutex)) return failure; \
    host_guard = &g_host_mutex

static uint32_t const spu_base = AFX_SPU_RAM_BASE_SH4;

static void upload_dma_complete(void *ignored) {
    (void)ignored;
    g_upload_dma_done = true;
}
static void upload_dma_wait(void) {
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
static void *upload_image_alloc(uint32_t size) {
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

static bool grow(void **storage, uint32_t *capacity, uint32_t need, size_t item) {
    if (need <= *capacity) return true;
    uint32_t next = *capacity ? *capacity : 16;
    while (next < need) {
        if (next > UINT32_MAX / 2u) { next = need; break; }
        next *= 2u;
    }
    if ((size_t)next > SIZE_MAX / item) return false;
    void *grown = realloc(*storage, (size_t)next * item);
    if (!grown) return false;
    memset((uint8_t *)grown + (size_t)(*capacity) * item, 0,
           (size_t)(next - *capacity) * item);
    *storage = grown;
    *capacity = next;
    return true;
}
static bool reserve_free(uint32_t need) {
    return grow((void **)&g_free_blocks, &g_free_capacity, need, sizeof(*g_free_blocks));
}
static bool reserve_allocs(uint32_t need) {
    return grow((void **)&g_allocs, &g_alloc_capacity, need, sizeof(*g_allocs));
}
static bool reserve_assets(uint32_t need) {
    return need <= UINT16_MAX &&
           grow((void **)&g_assets, &g_asset_capacity, need, sizeof(*g_assets));
}
static uint32_t align_up(uint32_t value, uint32_t align) {
    if (!align || (align & (align - 1u)) || value > UINT32_MAX - (align - 1u))
        return 0;
    return (value + align - 1u) & ~(align - 1u);
}
static bool in_asset_arena(uint32_t address, uint32_t size) {
    return g_dynamic_base && size && address >= g_dynamic_base &&
           address < AFX_ASSET_LIMIT && afx_range(address, size, AFX_ASSET_LIMIT);
}
static bool allocation_diagnostic(uint32_t size, uint32_t align, afx_mem_diagnostic_t *out) {
    if (!g_ready || !out || !size || (align && (align & (align - 1u)))) return false;
    uint32_t use_align = align > AFX_UPLOAD_ALIGN ? align : AFX_UPLOAD_ALIGN;
    uint32_t aligned = align_up(size, AFX_UPLOAD_ALIGN);
    if (!aligned || use_align > AFX_ASSET_LIMIT) return false;
    memset(out, 0, sizeof(*out));
    out->requested_bytes = size;
    out->aligned_bytes = aligned;
    bool fits = false;
    for (uint32_t i = 0; i < g_free_count; ++i) {
        uint32_t start = align_up(g_free_blocks[i].addr, use_align);
        out->free_bytes += g_free_blocks[i].size;
        if (g_free_blocks[i].size > out->largest_free_block)
            out->largest_free_block = g_free_blocks[i].size;
        if (start && start - g_free_blocks[i].addr <= g_free_blocks[i].size &&
            aligned <= g_free_blocks[i].size - (start - g_free_blocks[i].addr)) {
            fits = true;
        }
    }
    out->result = fits ? AFX_MEM_AVAILABLE :
                  (out->free_bytes < aligned ? AFX_MEM_EXHAUSTED : AFX_MEM_FRAGMENTED);
    return true;
}
static uint32_t read_spu_word(uint32_t address) {
    return g2_read_32(spu_base + address);
}
/* spu_memload rounds inputs to words. A private aligned bounce prevents it from
 * reading after a short, unaligned caller buffer. */
static void upload_words(uint32_t address, const void *data, uint32_t size) {
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
static void maps_reset(void) {
    memset(g_map_used, 0, sizeof(g_map_used));
    uint32_t invalid[AFX_CHANNEL_MAP_ENTRIES];
    for (uint32_t i = 0; i < AFX_CHANNEL_MAP_ENTRIES; ++i) invalid[i] = UINT32_MAX;
    for (uint32_t arena = 0; arena < AFX_CHANNEL_MAP_ARENAS; ++arena)
        upload_words(AFX_CHANNEL_MAP_ARENA_ADDR + arena * AFX_CHANNEL_MAP_ARENA_SIZE,
                     invalid, sizeof(invalid));
}
static bool allocator_reset(uint32_t dynamic_base) {
    upload_dma_wait();
    g_dynamic_base = 0;
    g_free_count = g_alloc_count = 0;
    for (uint32_t i = 0; i < g_asset_capacity; ++i) {
        free(g_assets[i].checkpoints);
        free(g_assets[i].image);
        free(g_assets[i].dependencies);
        g_assets[i].dependencies = NULL;
        g_assets[i].dependency_count = 0;
        g_assets[i].sample = g_assets[i].sfx = false;
        g_assets[i].sample_bank = false;
        g_assets[i].owns_allocation = false;
        g_assets[i].backing = AFX_ASSET_INVALID;
        g_assets[i].sample_frames = g_assets[i].sample_format = 0;
        g_assets[i].checkpoints = NULL;
        g_assets[i].checkpoints_size = 0;
        g_assets[i].image = NULL;
        if (g_assets[i].live) {
            if (g_assets[i].generation == UINT16_MAX) g_assets[i].retired = true;
            else ++g_assets[i].generation;
        }
        g_assets[i].live = false;
        g_assets[i].flow = false;
        g_assets[i].uploading = false;
        g_assets[i].references = 0;
        g_assets[i].peak_commands = g_assets[i].peak_register_writes = 0;
        g_assets[i].upload_cursor = 0;
    }
    for (uint32_t i = 0; i < AFX_MAX_FLOW_SLOTS; ++i) {
        uint16_t generation = g_instances[i].generation;
        bool retired = g_instances[i].retired;
        if (g_instances[i].live) {
            if (generation == UINT16_MAX) retired = true;
            else ++generation;
        }
        memset(&g_instances[i], 0, sizeof(g_instances[i]));
        g_instances[i].generation = generation;
        g_instances[i].retired = retired;
    }
    g_available_channels = UINT64_MAX;
    g_reserved_peak_commands = g_reserved_peak_writes = 0;
    g_dsp_scene = false;
    g_dsp_return_left = g_dsp_return_right = 0;
    g_next_sequence = 1;
    if (!dynamic_base || dynamic_base >= AFX_ASSET_LIMIT || !reserve_free(1))
        return false;
    g_dynamic_base = dynamic_base;
    g_free_blocks[0] = (afx_block_t){dynamic_base, AFX_ASSET_LIMIT - dynamic_base};
    g_free_count = 1;
    return true;
}
static bool insert_free_block(uint32_t address, uint32_t size) {
    if (!in_asset_arena(address, size)) return false;
    uint32_t index = 0;
    while (index < g_free_count && g_free_blocks[index].addr < address) ++index;
    if ((index && g_free_blocks[index - 1].addr + g_free_blocks[index - 1].size > address) ||
        (index < g_free_count && address + size > g_free_blocks[index].addr))
        return false;
    if (index && g_free_blocks[index - 1].addr + g_free_blocks[index - 1].size == address) {
        g_free_blocks[index - 1].size += size;
        if (index < g_free_count && address + size == g_free_blocks[index].addr) {
            g_free_blocks[index - 1].size += g_free_blocks[index].size;
            memmove(&g_free_blocks[index], &g_free_blocks[index + 1],
                    (g_free_count - index - 1u) * sizeof(*g_free_blocks));
            --g_free_count;
        }
        return true;
    }
    if (index < g_free_count && address + size == g_free_blocks[index].addr) {
        g_free_blocks[index].addr = address;
        g_free_blocks[index].size += size;
        return true;
    }
    if (!reserve_free(g_free_count + 1)) return false;
    memmove(&g_free_blocks[index + 1], &g_free_blocks[index],
            (g_free_count - index) * sizeof(*g_free_blocks));
    g_free_blocks[index] = (afx_block_t){address, size};
    ++g_free_count;
    return true;
}
uint32_t afx_mem_alloc(uint32_t size, uint32_t align) {
    HOST_GUARD(0);
    if (!g_ready || !size || (align && (align & (align - 1u)))) return 0;
    uint32_t use_align = align > AFX_UPLOAD_ALIGN ? align : AFX_UPLOAD_ALIGN;
    size = align_up(size, AFX_UPLOAD_ALIGN);
    if (!size || use_align > AFX_ASSET_LIMIT || !reserve_allocs(g_alloc_count + 1))
        return 0;

    uint32_t best = UINT32_MAX, best_start = 0, best_pad = 0, best_remain = UINT32_MAX;
    for (uint32_t i = 0; i < g_free_count; ++i) {
        uint32_t start = align_up(g_free_blocks[i].addr, use_align);
        if (!start) continue;
        uint32_t pad = start - g_free_blocks[i].addr;
        if (pad > g_free_blocks[i].size || size > g_free_blocks[i].size - pad) continue;
        uint32_t remain = g_free_blocks[i].size - pad - size;
        if (remain < best_remain) {
            best = i; best_start = start; best_pad = pad; best_remain = remain;
        }
    }
    if (best == UINT32_MAX) return 0;
    if (best_pad && best_remain && !reserve_free(g_free_count + 1)) return 0;

    if (!best_pad && !best_remain) {
        memmove(&g_free_blocks[best], &g_free_blocks[best + 1],
                (g_free_count - best - 1u) * sizeof(*g_free_blocks));
        --g_free_count;
    } else if (!best_pad) {
        g_free_blocks[best] = (afx_block_t){best_start + size, best_remain};
    } else if (!best_remain) {
        g_free_blocks[best].size = best_pad;
    } else {
        memmove(&g_free_blocks[best + 2], &g_free_blocks[best + 1],
                (g_free_count - best - 1u) * sizeof(*g_free_blocks));
        g_free_blocks[best].size = best_pad;
        g_free_blocks[best + 1] = (afx_block_t){best_start + size, best_remain};
        ++g_free_count;
    }
    g_allocs[g_alloc_count++] = (afx_block_t){best_start, size};
    return best_start;
}
static bool free_allocation(uint32_t address) {
    if (!g_ready) return false;
    for (uint32_t i = 0; i < g_alloc_count; ++i) {
        if (g_allocs[i].addr != address) continue;
        if (!insert_free_block(address, g_allocs[i].size)) return false;
        memmove(&g_allocs[i], &g_allocs[i + 1],
                (g_alloc_count - i - 1u) * sizeof(*g_allocs));
        --g_alloc_count;
        return true;
    }
    return false;
}
int afx_mem_free(uint32_t address) {
    HOST_GUARD(-AFX_BUSY);
    for (uint32_t i = 0; i < g_asset_capacity; ++i)
        if ((g_assets[i].live || g_assets[i].uploading) && g_assets[i].addr == address)
            return -AFX_ASSET_REFERENCED;
    for (uint32_t i = 0; i < AFX_MAX_FLOW_SLOTS; ++i)
        if (g_instances[i].live && g_instances[i].staging_addr == address)
            return -AFX_ASSET_REFERENCED;
    return free_allocation(address) ? AFX_OK : -AFX_BAD_BOUNDS;
}
int afx_mem_upload(uint32_t address, const void *data, uint32_t size) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t rounded = align_up(size, 4);
    if (!g_ready || !data || !size || !rounded || (address & 3) ||
        !in_asset_arena(address, rounded)) return -AFX_BAD_BOUNDS;
    for (uint32_t i = 0; i < g_alloc_count; ++i)
        if (address >= g_allocs[i].addr &&
            afx_range(address - g_allocs[i].addr, rounded, g_allocs[i].size)) {
            upload_words(address, data, size);
            return AFX_OK;
        }
    return -AFX_BAD_BOUNDS;
}
int afx_mem_stats(afx_mem_stats_t *out) {
    HOST_GUARD(-AFX_BUSY);
    if (!g_ready || !out) return -AFX_BAD_BOUNDS;
    memset(out, 0, sizeof(*out));
    out->dynamic_base = g_dynamic_base;
    out->asset_limit = AFX_ASSET_LIMIT;
    out->total_bytes = AFX_ASSET_LIMIT - g_dynamic_base;
    out->active_allocations = g_alloc_count;
    out->free_block_count = g_free_count;
    for (uint32_t i = 0; i < g_free_count; ++i) {
        out->free_bytes += g_free_blocks[i].size;
        if (g_free_blocks[i].size > out->largest_free_block)
            out->largest_free_block = g_free_blocks[i].size;
    }
    out->used_bytes = out->total_bytes - out->free_bytes;
    return AFX_OK;
}
int afx_mem_diagnose(uint32_t size, uint32_t align, afx_mem_diagnostic_t *out) {
    HOST_GUARD(-AFX_BUSY);
    return allocation_diagnostic(size, align, out) ? AFX_OK : -AFX_BAD_BOUNDS;
}
static bool resolve_asset(afx_asset_t asset, uint32_t *index) {
    if (!asset || !index) return false;
    uint32_t raw = asset & 0xffffu;
    if (!raw || raw > g_asset_capacity) return false;
    afx_asset_slot_t *slot = &g_assets[raw - 1u];
    if ((!slot->live && !slot->uploading) || slot->generation != AFX_HANDLE_GENERATION(asset)) return false;
    *index = raw - 1u;
    return true;
}
static afx_asset_t reserve_asset(uint32_t size, uint32_t align, bool live) {
    uint32_t index = 0;
    uint32_t allocation_size;
    while (index < g_asset_capacity &&
           (g_assets[index].live || g_assets[index].uploading || g_assets[index].retired)) ++index;
    if (index == g_asset_capacity && !reserve_assets(g_asset_capacity + 1)) return 0;
    allocation_size = align_up(size, align);
    uint32_t address = allocation_size ? afx_mem_alloc(allocation_size, align) : 0;
    if (!address) return 0;
    afx_asset_slot_t *slot = &g_assets[index];
    if (!slot->generation) slot->generation = 1;
    slot->live = live;
    slot->uploading = !live;
    slot->addr = address;
    slot->size = size;
    slot->allocation_size = allocation_size;
    slot->upload_cursor = 0;
    slot->owns_allocation = true;
    slot->backing = AFX_ASSET_INVALID;
    slot->sample_bank = false;
    return AFX_MAKE_HANDLE(index, slot->generation);
}
afx_asset_t afx_asset_upload(const void *data, uint32_t size, uint32_t align) {
    HOST_GUARD(0);
    if (!data || !size) return 0;
    afx_asset_t asset = reserve_asset(size, align, true);
    if (!asset) return 0;
    if (afx_mem_upload(afx_asset_addr(asset), data, size)) {
        (void)afx_asset_free(asset);
        return 0;
    }
    return asset;
}
int afx_asset_free(afx_asset_t asset) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_asset(asset, &index)) return -AFX_INVALID_HANDLE;
    afx_asset_slot_t *slot = &g_assets[index];
    if (g_upload_dma_asset == asset) upload_dma_wait();
    if (slot->references) return -AFX_ASSET_REFERENCED;
    if (slot->owns_allocation && !free_allocation(slot->addr)) return -AFX_BAD_BOUNDS;
    for (uint32_t i = 0; i < slot->dependency_count; ++i) {
        uint32_t dependency;
        if (resolve_asset(slot->dependencies[i], &dependency)) --g_assets[dependency].references;
    }
    free(slot->dependencies);
    slot->dependencies = NULL;
    slot->dependency_count = 0;
    if (slot->backing) {
        uint32_t backing;
        if (resolve_asset(slot->backing, &backing) && g_assets[backing].references)
            --g_assets[backing].references;
    }
    slot->sample = slot->sfx = slot->sample_bank = false;
    slot->sample_frames = slot->sample_format = 0;
    free(slot->checkpoints);
    if (slot->image_owned) free(slot->image);
    slot->checkpoints = NULL;
    slot->checkpoints_size = 0;
    slot->image = NULL;
    slot->live = false;
    slot->flow = false;
    slot->uploading = false;
    slot->addr = slot->size = slot->allocation_size = 0;
    slot->owns_allocation = false;
    slot->backing = AFX_ASSET_INVALID;
    slot->upload_cursor = 0;
    slot->peak_commands = slot->peak_register_writes = 0;
    if (slot->generation == UINT16_MAX) slot->retired = true;
    else ++slot->generation;
    return AFX_OK;
}
uint32_t afx_asset_addr(afx_asset_t asset) {
    HOST_GUARD(0);
    uint32_t index;
    return resolve_asset(asset, &index) ? g_assets[index].addr : 0;
}
uint32_t afx_asset_size(afx_asset_t asset) {
    HOST_GUARD(0);
    uint32_t index;
    return resolve_asset(asset, &index) ? g_assets[index].size : 0;
}
int afx_asset_work(afx_asset_t asset, afx_work_profile_t *out) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!out || !resolve_asset(asset, &index) || !g_assets[index].flow) return -AFX_INVALID_HANDLE;
    out->peak_commands = g_assets[index].peak_commands;
    out->peak_register_writes = g_assets[index].peak_register_writes;
    return AFX_OK;
}

static int upload_sample_data(const void *data, uint32_t bytes, afx_asset_t *out) {
    void *staging;
    uint32_t padded;
    padded = align_up(bytes, AFX_UPLOAD_ALIGN);
    staging = upload_image_alloc(bytes);
    if (!padded || !staging || g_upload_dma_asset) {
        free(staging);
        return g_upload_dma_asset ? -AFX_BUSY : -AFX_NO_HOST_RAM;
    }
    memcpy(staging, data, bytes);
    afx_asset_t asset = reserve_asset(bytes, AFX_UPLOAD_ALIGN, true);
    if (!asset) { free(staging); return -AFX_NO_AICA_RAM; }
    dcache_wback_range((uintptr_t)staging, padded);
    g_upload_dma_asset = asset;
    g_upload_dma_bytes = bytes;
    g_upload_dma_offset = 0;
    g_upload_dma_stream = false;
    g_upload_dma_done = false;
    if (spu_dma_transfer(staging, afx_asset_addr(asset), padded, 0, upload_dma_complete, NULL)) {
        g_upload_dma_asset = AFX_ASSET_INVALID;
        g_upload_dma_bytes = 0;
        free(staging);
        (void)afx_asset_free(asset);
        return -AFX_BUSY;
    }
    upload_dma_wait();
    free(staging);
    uint32_t index;
    (void)resolve_asset(asset, &index);
    *out = asset;
    return AFX_OK;
}

int afx_sample_upload(const void *data, uint32_t bytes, uint32_t frames,
                       uint32_t format, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!data || !bytes || !frames || bytes > AFX_ASSET_LIMIT || format > AFX_ADPCM ||
        (format == AFX_PCM16 && ((bytes & 1u) || frames > bytes / 2)) ||
        (format == AFX_PCM8 && frames > bytes) ||
        (format == AFX_ADPCM && frames / 2 + (frames & 1u) > bytes)) return -AFX_BAD_SAMPLE;
    int result = upload_sample_data(data, bytes, out);
    if (!result) {
        uint32_t index;
        (void)resolve_asset(*out, &index);
        g_assets[index].sample = true;
        g_assets[index].sample_frames = frames;
        g_assets[index].sample_format = format;
    }
    return result;
}

int afx_sample_bank_upload(const void *data, uint32_t bytes, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!data || !bytes || bytes > AFX_ASSET_LIMIT) return -AFX_BAD_BOUNDS;
    int result = upload_sample_data(data, bytes, out);
    if (!result) {
        uint32_t index;
        (void)resolve_asset(*out, &index);
        g_assets[index].sample_bank = true;
    }
    return result;
}

int afx_sample_view_create(afx_asset_t bank, uint32_t byte_offset, uint32_t bytes,
                           uint32_t frames, uint32_t format, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t backing, index = 0;
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!resolve_asset(bank, &backing) || !g_assets[backing].sample_bank ||
        g_assets[backing].uploading || !bytes || !frames || format > AFX_ADPCM ||
        (byte_offset & (AFX_UPLOAD_ALIGN - 1u)) || byte_offset > g_assets[backing].size ||
        bytes > g_assets[backing].size - byte_offset ||
        (format == AFX_PCM16 && ((bytes & 1u) || frames > bytes / 2)) ||
        (format == AFX_PCM8 && frames > bytes) ||
        (format == AFX_ADPCM && frames / 2 + (frames & 1u) > bytes)) return -AFX_BAD_SAMPLE;
    while (index < g_asset_capacity &&
           (g_assets[index].live || g_assets[index].uploading || g_assets[index].retired)) ++index;
    if (index == g_asset_capacity && !reserve_assets(g_asset_capacity + 1)) return -AFX_NO_HOST_RAM;
    afx_asset_slot_t *slot = &g_assets[index];
    if (!slot->generation) slot->generation = 1;
    slot->live = true;
    slot->uploading = slot->flow = false;
    slot->sample = true;
    slot->sfx = false;
    slot->addr = g_assets[backing].addr + byte_offset;
    slot->size = bytes;
    slot->allocation_size = 0;
    slot->sample_frames = frames;
    slot->sample_format = format;
    slot->owns_allocation = false;
    slot->backing = bank;
    ++g_assets[backing].references;
    *out = AFX_MAKE_HANDLE(index, slot->generation);
    return AFX_OK;
}

int afx_sample_bank_stream_begin(uint32_t bytes, afx_asset_t *out) {
    uint32_t index;
    afx_asset_t asset;
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!bytes || bytes > AFX_ASSET_LIMIT) return -AFX_BAD_BOUNDS;
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

static uint64_t allocate_channels(uint8_t count) {
    if (!count || count > 64) return 0;
    uint64_t picked = 0;
    for (uint32_t bit = 0; bit < 64 && count; ++bit)
        if (g_available_channels & (UINT64_C(1) << bit)) {
            picked |= UINT64_C(1) << bit;
            --count;
        }
    if (count) return 0;
    g_available_channels &= ~picked;
    return picked;
}
static bool map_allocate(uint8_t count, uint64_t channels, uint32_t *address,
                         uint8_t *arena_out, uint8_t *offset_out) {
    uint8_t arena = count <= 4 ? 0 : count <= 8 ? 1 : count <= 16 ? 2 : count <= 32 ? 3 : 4;
    uint64_t needed = count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1u;
    for (uint32_t offset = 0; offset + count <= AFX_CHANNEL_MAP_ENTRIES; ++offset) {
        uint64_t placed = needed << offset;
        if (g_map_used[arena] & placed) continue;
        g_map_used[arena] |= placed;
        uint32_t base = AFX_CHANNEL_MAP_ARENA_ADDR + arena * AFX_CHANNEL_MAP_ARENA_SIZE +
                        offset * AFX_CHANNEL_MAP_ENTRY_BYTES;
        uint32_t entries[AFX_CHANNEL_MAP_ENTRIES];
        uint32_t local = 0;
        for (uint32_t physical = 0; physical < 64; ++physical)
            if (channels & (UINT64_C(1) << physical)) {
                entries[local++] = physical;
            }
        upload_words(base, entries, count * sizeof(*entries));
        *address = base;
        *arena_out = arena;
        *offset_out = offset;
        return true;
    }
    return false;
}
static void map_release(const afx_instance_slot_t *slot) {
    if (!slot->required_channels) return;
    uint64_t mask = slot->required_channels == 64 ? UINT64_MAX :
                    (UINT64_C(1) << slot->required_channels) - 1u;
    g_map_used[slot->map_arena] &= ~(mask << slot->map_offset);
    uint32_t invalid[AFX_CHANNEL_MAP_ENTRIES];
    for (uint32_t i = 0; i < slot->required_channels; ++i) invalid[i] = UINT32_MAX;
    upload_words(slot->map_addr, invalid, slot->required_channels * sizeof(*invalid));
}
static uint32_t new_sequence(void) {
    if (!g_next_sequence) ++g_next_sequence;
    return g_next_sequence++;
}
static int enqueue(uint32_t opcode, uint32_t reference, uint32_t sequence,
                   uint32_t flags, const void *payload, uint32_t payload_size) {
    if (!g_ready || !g_lifecycle || payload_size > 48) return -AFX_UNSUPPORTED;
    /* The AICA sees the command only after the G2 FIFO has drained. This is
     * the same payload-then-head discipline as KOS's sound command queue. */
    g2_lock_scoped();
    int result = AFX_OK;
    uint32_t head = g2_read_32_raw(spu_base + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, head));
    uint32_t tail = g2_read_32_raw(spu_base + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail));
    if (head - tail > AFX_CMD_QUEUE_CAPACITY) result = -AFX_BAD_FIRMWARE;
    else if (head - tail == AFX_CMD_QUEUE_CAPACITY) result = -AFX_IPC_FULL;
    else {
        afx_cmd_t command = { .opcode = opcode, .sequence = sequence,
                              .reference = reference, .flags = flags };
        if (payload_size) memcpy(command.payload, payload, payload_size);
        uint32_t address = AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, commands) +
                           (head & (AFX_CMD_QUEUE_CAPACITY - 1u)) * sizeof(command);
        const uint32_t *words = (const uint32_t *)&command;
        for (uint32_t i = 0; i < sizeof(command) / 4; ++i) {
            if (!(i & 7u)) g2_fifo_wait();
            g2_write_32_raw(spu_base + address + i * 4, words[i]);
        }
        g2_fifo_wait();
        g2_write_32_raw(spu_base + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, head), head + 1u);
    }
    return result;
}
static bool read_observed(uint32_t index, afx_instance_status_t *out) {
    if (index >= AFX_MAX_FLOW_SLOTS || !out) return false;
    uint32_t base = AFX_OBSERVED_ADDR + index * sizeof(afx_observed_t);
    uint32_t first = read_spu_word(base);
    if (first & 1u) return false;
    afx_instance_status_t copy = {
        .reference = read_spu_word(base + 4), .state = read_spu_word(base + 8),
        .sequence = read_spu_word(base + 12), .result = read_spu_word(base + 16),
        .position = read_spu_word(base + 20), .next_deadline = read_spu_word(base + 24),
        .detail = read_spu_word(base + 28)
    };
    if (first != read_spu_word(base) || (first & 1u)) return false;
    *out = copy;
    return true;
}
static bool resolve_instance(afx_instance_t instance, uint32_t *index) {
    if (!instance || !index) return false;
    uint32_t raw = instance & 0xffffu;
    if (!raw || raw > AFX_MAX_FLOW_SLOTS) return false;
    afx_instance_slot_t *slot = &g_instances[raw - 1u];
    if (!slot->live || slot->generation != AFX_HANDLE_GENERATION(instance)) return false;
    *index = raw - 1u;
    return true;
}
static void release_instance_work(afx_instance_slot_t *slot) {
    if (!slot->work_reserved) return;
    const afx_asset_slot_t *flow = &g_assets[slot->asset_index];
    g_reserved_peak_commands -= flow->peak_commands;
    g_reserved_peak_writes -= flow->peak_register_writes;
    slot->work_reserved = false;
}
static void release_instance_preserving_generation(uint32_t index) {
    afx_instance_slot_t *slot = &g_instances[index];
    uint16_t generation = slot->generation;
    bool retired = slot->retired;
    if (slot->staging_addr) (void)free_allocation(slot->staging_addr);
    g_available_channels |= slot->channel_mask;
    map_release(slot);
    if (slot->asset_index < g_asset_capacity && g_assets[slot->asset_index].references)
        --g_assets[slot->asset_index].references;
    release_instance_work(slot);
    if (generation == UINT16_MAX) retired = true;
    else ++generation;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
    slot->retired = retired;
}

int afx_init(const void *firmware, uint32_t size) {
    afx_firmware_info_t info;
    afx_result_t valid = afx_firmware_validate(firmware, size, &info);
    if (valid) return -(int)valid; /* No reset on rejected image. */
    g_ready = g_lifecycle = false;
    spu_disable();
    spu_memset(0, 0, AFX_AICA_RAM_SIZE);
    upload_words(0, firmware, size);
    spu_enable();
    uint64_t started = timer_ms_gettime64();
    while (timer_ms_gettime64() - started < 1000) {
        uint32_t capabilities = read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, capabilities));
        if (read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, magic)) == AFX_STATUS_MAGIC &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, abi)) == AFX_ABI_VERSION &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, layout_id)) == AFX_LAYOUT_ID &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, asset_base)) == info.asset_base &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, asset_limit)) == info.asset_limit &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, private_end)) == info.private_end &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, stack_base)) == info.stack_base &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, error)) == AFX_OK &&
            (capabilities & AFX_CAP_BOOTSTRAP)) {
            if (!allocator_reset(info.asset_base)) { spu_disable(); return -AFX_NO_HOST_RAM; }
            maps_reset();
            g_ready = true;
            g_lifecycle = (capabilities & AFX_CAP_LIFECYCLE) != 0;
            return AFX_OK;
        }
    }
    spu_disable();
    g_dynamic_base = 0;
    return -AFX_TIMEOUT;
}
void afx_shutdown(void) {
    g_ready = g_lifecycle = false;
    spu_disable();
    (void)allocator_reset(0);
}
int afx_flow_upload_begin(const void *data, uint32_t size, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    afx_file_header_t header;
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    afx_result_t valid = afx_file_validate(data, size, &header);
    if (valid) return -(int)valid;
    uint8_t *checkpoints = NULL;
    if (header.checkpoints_size) {
        checkpoints = malloc(header.checkpoints_size);
        if (!checkpoints) return -AFX_NO_HOST_RAM;
        memcpy(checkpoints, (const uint8_t *)data + header.checkpoints_offset,
               header.checkpoints_size);
    }
    uint8_t *linked = upload_image_alloc(header.image_size);
    if (!linked) { free(checkpoints); return -AFX_NO_HOST_RAM; }
    afx_mem_diagnostic_t diagnostic;
    if (!allocation_diagnostic(header.image_size, AFX_UPLOAD_ALIGN, &diagnostic)) {
        free(linked); free(checkpoints); return -AFX_BAD_BOUNDS;
    }
    if (diagnostic.result != AFX_MEM_AVAILABLE) {
        free(linked); free(checkpoints); return -AFX_NO_AICA_RAM;
    }
    afx_asset_t asset = reserve_asset(header.image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) { free(linked); free(checkpoints); return -AFX_NO_AICA_RAM; }
    valid = afx_link_validated_image(data, &header, afx_asset_addr(asset), linked, header.image_size);
    uint32_t peak_commands = 0, peak_writes = 0;
    if (!valid) valid = profile_stream(&header, linked, &peak_commands, &peak_writes);
    if (!valid && header.work_profile &&
        (AFX_WORK_PROFILE_COMMANDS(header.work_profile) != peak_commands ||
         AFX_WORK_PROFILE_WRITES(header.work_profile) != peak_writes)) valid = AFX_BAD_FORMAT;
    if (valid) { (void)afx_asset_free(asset); free(linked); free(checkpoints); return -(int)valid; }
    uint32_t index;
    (void)resolve_asset(asset, &index);
    g_assets[index].header = header;
    g_assets[index].checkpoints = checkpoints;
    g_assets[index].checkpoints_size = header.checkpoints_size;
    g_assets[index].image = linked;
    g_assets[index].image_owned = true;
    g_assets[index].peak_commands = peak_commands;
    g_assets[index].peak_register_writes = peak_writes;
    *out = asset;
    return AFX_OK;
}
int afx_external_flow_begin(const afx_external_flow_t *recipe, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!g_ready || !recipe || !recipe->stream || !recipe->stream_size ||
        (recipe->setup_count && !recipe->setups) ||
        ((recipe->flags & AFX_FLAG_LANES) && !recipe->lane_map) ||
        recipe->setup_count > 65536 || recipe->setup_count > AFX_ASSET_LIMIT / AFX_SETUP_BYTES ||
        recipe->required_channels > AFX_MAX_FLOW_CHANNELS)
        return -AFX_BAD_BOUNDS;
    uint32_t lane_bytes = (recipe->flags & AFX_FLAG_LANES) ? recipe->required_channels : 0;
    uint32_t fixed_bytes = recipe->setup_count * AFX_SETUP_BYTES + lane_bytes;
    if (fixed_bytes > AFX_ASSET_LIMIT || recipe->stream_size > AFX_ASSET_LIMIT - fixed_bytes)
        return -AFX_BAD_BOUNDS;
    afx_file_header_t header = {
        .magic = AFX_FILE_MAGIC, .abi = AFX_ABI_VERSION, .flags = recipe->flags,
        .image_size = recipe->setup_count * AFX_SETUP_BYTES + lane_bytes + recipe->stream_size,
        .setup_count = recipe->setup_count,
        .stream_offset = recipe->setup_count * AFX_SETUP_BYTES + lane_bytes,
        .stream_size = recipe->stream_size, .required_channels = recipe->required_channels,
        .tick_rate_num = recipe->tick_rate_num, .tick_rate_den = recipe->tick_rate_den
    };
    uint8_t *image = upload_image_alloc(header.image_size);
    afx_sample_t *samples = recipe->setup_count ? calloc(recipe->setup_count, sizeof(*samples)) : NULL;
    afx_asset_t *dependencies = recipe->setup_count ? malloc(recipe->setup_count * sizeof(*dependencies)) : NULL;
    int result = -AFX_NO_HOST_RAM;
    if (!image || (recipe->setup_count && (!samples || !dependencies))) goto failed;
    if (lane_bytes) memcpy(image + recipe->setup_count * AFX_SETUP_BYTES, recipe->lane_map, lane_bytes);
    memcpy(image + header.stream_offset, recipe->stream, recipe->stream_size);
    for (uint32_t i = 0; i < recipe->setup_count; ++i) {
        const afx_sfx_setup_t *setup = &recipe->setups[i];
        uint16_t state[AFX_FIELD_COUNT];
        memcpy(state, setup->fields, sizeof(state));
        result = -AFX_BAD_SAMPLE;
        if ((state[0] & 0x1ffu) || state[1]) goto failed;
        if (state[0] & 0x400u) {
            if (setup->sample || setup->byte_offset) goto failed;
        } else {
            uint32_t index;
            if (!resolve_asset(setup->sample, &index) || !g_assets[index].sample) {
                result = -AFX_INVALID_HANDLE; goto failed;
            }
            const afx_asset_slot_t *sample = &g_assets[index];
            if (setup->byte_offset >= sample->size ||
                (sample->sample_format == AFX_PCM16 && (setup->byte_offset & 1u)) ||
                (sample->sample_format == AFX_ADPCM && setup->byte_offset)) goto failed;
            uint32_t address = sample->addr + setup->byte_offset;
            state[0] |= (uint16_t)((sample->sample_format << 7) | (address >> 16));
            state[1] = (uint16_t)address;
            uint32_t j = 0;
            while (j < header.sample_count && dependencies[j] != setup->sample) ++j;
            if (j == header.sample_count) {
                dependencies[j] = setup->sample;
                samples[j] = (afx_sample_t){sample->addr, sample->size, sample->sample_frames, sample->sample_format};
                ++header.sample_count;
            }
        }
        for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field)
            afx_write16(image + i * AFX_SETUP_BYTES + field * 2, state[field]);
    }
    result = -(int)afx_sfx_validate(image, &header, samples);
    if (result) goto failed;
    uint32_t commands, writes;
    result = -(int)profile_stream(&header, image, &commands, &writes);
    if (result) goto failed;
    afx_asset_t asset = reserve_asset(header.image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) { result = -AFX_NO_AICA_RAM; goto failed; }
    uint32_t index;
    (void)resolve_asset(asset, &index);
    afx_asset_slot_t *flow = &g_assets[index];
    flow->sfx = !(header.flags & AFX_FLAG_MUSIC);
    flow->header = header;
    flow->image = image;
    flow->image_owned = true;
    flow->dependencies = dependencies;
    flow->dependency_count = header.sample_count;
    flow->peak_commands = commands;
    flow->peak_register_writes = writes;
    for (uint32_t i = 0; i < header.sample_count; ++i) {
        uint32_t dependency;
        (void)resolve_asset(dependencies[i], &dependency);
        ++g_assets[dependency].references;
    }
    free(samples);
    *out = asset;
    return AFX_OK;
failed:
    free(image); free(samples); free(dependencies);
    return result;
}

int afx_flow_upload_step(afx_asset_t asset, uint32_t max_bytes,
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
int afx_flow_upload_dma_step(afx_asset_t asset, uint32_t max_bytes,
                             uint32_t *out_uploaded, bool *out_complete) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (out_uploaded) *out_uploaded = 0;
    if (out_complete) *out_complete = false;
    if (!max_bytes || !resolve_asset(asset, &index) || !g_assets[index].uploading)
        return -AFX_BAD_BOUNDS;
    afx_asset_slot_t *slot = &g_assets[index];
    if (g_upload_dma_asset) {
        if (g_upload_dma_asset != asset) return -AFX_BUSY;
        if (!g_upload_dma_done) return AFX_OK;
        uint32_t uploaded = g_upload_dma_bytes;
        upload_dma_wait();
        slot->upload_cursor += uploaded;
        if (out_uploaded) *out_uploaded = uploaded;
    }
    if (slot->upload_cursor == slot->size) {
        slot->uploading = false;
        slot->live = slot->flow = true;
        if (out_complete) *out_complete = true;
        return AFX_OK;
    }
    uint32_t remaining = slot->size - slot->upload_cursor;
    uint32_t amount = remaining < max_bytes ? remaining : max_bytes;
    if (amount < remaining) amount &= ~(AFX_UPLOAD_ALIGN - 1u);
    if (!amount) return -AFX_BAD_BOUNDS;
    /* Assets and staged sources are aligned and their padding is zeroed, so the
     * final DMA includes only harmless local padding rather than using PIO. */
    uint32_t dma_bytes = amount == remaining ? align_up(amount, AFX_UPLOAD_ALIGN) : amount;
    dcache_wback_range((uintptr_t)slot->image + slot->upload_cursor, dma_bytes);
    g_upload_dma_asset = asset;
    g_upload_dma_bytes = amount;
    g_upload_dma_stream = false;
    g_upload_dma_done = false;
    if (spu_dma_transfer(slot->image + slot->upload_cursor,
                         slot->addr + slot->upload_cursor, dma_bytes, 0,
                         upload_dma_complete, NULL)) {
        g_upload_dma_asset = AFX_ASSET_INVALID;
        g_upload_dma_bytes = 0;
        return -AFX_BUSY;
    }
    return AFX_OK;
}
static int finish_flow_upload(afx_asset_t asset, afx_asset_t *out) {
    bool complete = false;
    while (!complete) {
        int result = afx_flow_upload_step(asset, AFX_UPLOAD_STEP_BYTES, NULL, &complete);
        if (result) { (void)afx_asset_free(asset); return result; }
    }
    *out = asset;
    return AFX_OK;
}
int afx_flow_upload(const void *data, uint32_t size, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    afx_asset_t asset;
    if (!out) return -AFX_BAD_BOUNDS;
    int result = afx_flow_upload_begin(data, size, &asset);
    return result ? result : finish_flow_upload(asset, out);
}
int afx_flow_upload_inplace(void *data, uint32_t size, afx_asset_t *out) {
    afx_file_header_t header;
    uint8_t *checkpoints = NULL;
    uint32_t commands = 0, writes = 0, index;
    afx_asset_t asset;
    afx_result_t valid;
    int result;

    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    valid = afx_file_validate(data, size, &header);
    if (valid) return -(int)valid;
    if (header.checkpoints_size) {
        checkpoints = malloc(header.checkpoints_size);
        if (!checkpoints) return -AFX_NO_HOST_RAM;
        memcpy(checkpoints, (const uint8_t *)data + header.checkpoints_offset, header.checkpoints_size);
    }
    asset = reserve_asset(header.image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) { free(checkpoints); return -AFX_NO_AICA_RAM; }
    valid = afx_link_validated_image_inplace(data, &header, afx_asset_addr(asset), size);
    if (!valid) valid = profile_stream(&header, data, &commands, &writes);
    if (!valid && header.work_profile &&
        (AFX_WORK_PROFILE_COMMANDS(header.work_profile) != commands ||
         AFX_WORK_PROFILE_WRITES(header.work_profile) != writes)) valid = AFX_BAD_FORMAT;
    if (valid) { (void)afx_asset_free(asset); free(checkpoints); return -(int)valid; }
    (void)resolve_asset(asset, &index);
    g_assets[index].header = header;
    g_assets[index].checkpoints = checkpoints;
    g_assets[index].checkpoints_size = header.checkpoints_size;
    g_assets[index].image = data;
    g_assets[index].peak_commands = commands;
    g_assets[index].peak_register_writes = writes;
    result = finish_flow_upload(asset, out);
    return result;
}

int afx_flow_stream_begin(const afx_file_header_t *header, const void *checkpoints,
                          afx_asset_t *out) {
    uint32_t index;
    afx_asset_t asset;

    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    if (!header || !header->image_size || !header->stream_size ||
        header->stream_offset > header->image_size ||
        header->stream_size > header->image_size - header->stream_offset ||
        !header->required_channels || header->required_channels > AFX_MAX_FLOW_CHANNELS ||
        !header->tick_rate_num || !header->tick_rate_den ||
        (header->checkpoints_size && !checkpoints)) return -AFX_BAD_BOUNDS;
    asset = reserve_asset(header->image_size, AFX_UPLOAD_ALIGN, false);
    if (!asset) return -AFX_NO_AICA_RAM;
    (void)resolve_asset(asset, &index);
    g_assets[index].header = *header;
    if (header->checkpoints_size) {
        g_assets[index].checkpoints = malloc(header->checkpoints_size);
        if (!g_assets[index].checkpoints) { (void)afx_asset_free(asset); return -AFX_NO_HOST_RAM; }
        memcpy(g_assets[index].checkpoints, checkpoints, header->checkpoints_size);
        g_assets[index].checkpoints_size = header->checkpoints_size;
    }
    *out = asset;
    return AFX_OK;
}
int afx_flow_stream_upload(afx_asset_t asset, uint32_t image_offset,
                           const void *data, uint32_t size) {
    uint32_t index;

    HOST_GUARD(-AFX_BUSY);
    if (!data || !size || !resolve_asset(asset, &index) || g_assets[index].sample_bank ||
        !g_assets[index].uploading)
        return -AFX_BAD_BOUNDS;
    afx_asset_slot_t *flow = &g_assets[index];
    if (image_offset != flow->upload_cursor || size > flow->size - image_offset)
        return -AFX_BAD_BOUNDS;
    int result = afx_mem_upload(flow->addr + image_offset, data, size);
    if (result) return result;
    flow->upload_cursor += size;
    return AFX_OK;
}
int afx_flow_stream_upload_dma_begin(afx_asset_t asset, uint32_t image_offset,
                                     const void *data, uint32_t size) {
    uint32_t index, padded;

    HOST_GUARD(-AFX_BUSY);
    if (!data || !size || ((uintptr_t)data & (AFX_UPLOAD_ALIGN - 1u)) ||
        g_upload_dma_asset || !resolve_asset(asset, &index) || g_assets[index].sample_bank ||
        !g_assets[index].uploading)
        return -AFX_BAD_BOUNDS;
    afx_asset_slot_t *flow = &g_assets[index];
    padded = align_up(size, AFX_UPLOAD_ALIGN);
    if (!padded || image_offset != flow->upload_cursor || size > flow->size - image_offset ||
        padded > flow->allocation_size - image_offset) return -AFX_BAD_BOUNDS;
    dcache_wback_range((uintptr_t)data, padded);
    g_upload_dma_asset = asset;
    g_upload_dma_bytes = size;
    g_upload_dma_offset = image_offset;
    g_upload_dma_stream = true;
    g_upload_dma_done = false;
    if (spu_dma_transfer((void *)data, flow->addr + image_offset, padded, 0,
                         upload_dma_complete, NULL)) {
        g_upload_dma_asset = AFX_ASSET_INVALID;
        g_upload_dma_bytes = 0;
        g_upload_dma_offset = 0;
        g_upload_dma_stream = false;
        return -AFX_BUSY;
    }
    return AFX_OK;
}
int afx_flow_stream_upload_dma_poll(afx_asset_t asset, bool *out_complete) {
    uint32_t index;

    HOST_GUARD(-AFX_BUSY);
    if (out_complete) *out_complete = false;
    if (!resolve_asset(asset, &index) || g_assets[index].sample_bank ||
        !g_assets[index].uploading ||
        g_upload_dma_asset != asset || !g_upload_dma_stream) return -AFX_BAD_BOUNDS;
    if (!g_upload_dma_done) return AFX_OK;
    afx_asset_slot_t *flow = &g_assets[index];
    if (flow->upload_cursor != g_upload_dma_offset) return -AFX_BAD_BOUNDS;
    flow->upload_cursor += g_upload_dma_bytes;
    upload_dma_wait();
    if (out_complete) *out_complete = true;
    return AFX_OK;
}
int afx_flow_stream_finish(afx_asset_t asset) {
    uint32_t index;

    HOST_GUARD(-AFX_BUSY);
    if (!resolve_asset(asset, &index) || g_assets[index].sample_bank ||
        !g_assets[index].uploading) return -AFX_BAD_BOUNDS;
    if (g_upload_dma_asset == asset) return -AFX_BUSY;
    afx_asset_slot_t *flow = &g_assets[index];
    if (flow->upload_cursor != flow->size) return -AFX_BAD_BOUNDS;
    flow->peak_commands = AFX_WORK_PROFILE_COMMANDS(flow->header.work_profile);
    flow->peak_register_writes = AFX_WORK_PROFILE_WRITES(flow->header.work_profile);
    flow->uploading = false;
    flow->live = flow->flow = true;
    return AFX_OK;
}

int afx_flow_release_host_image(afx_asset_t asset) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_asset(asset, &index) || !g_assets[index].flow) return -AFX_INVALID_HANDLE;
    afx_asset_slot_t *flow = &g_assets[index];
    if (flow->uploading) return -AFX_BUSY;
    if (flow->image_owned) free(flow->image);
    flow->image = NULL;
    flow->image_owned = false;
    return AFX_OK;
}
int afx_external_flow_upload(const afx_external_flow_t *recipe, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    afx_asset_t asset;
    if (!out) return -AFX_BAD_BOUNDS;
    int result = afx_external_flow_begin(recipe, &asset);
    return result ? result : finish_flow_upload(asset, out);
}

int afx_sfx_flow_begin(const afx_sfx_flow_t *recipe, afx_asset_t *out) {
    if (!recipe || recipe->flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_LANES)) return -AFX_BAD_COMMAND;
    return afx_external_flow_begin(recipe, out);
}

int afx_sfx_flow_upload(const afx_sfx_flow_t *recipe, afx_asset_t *out) {
    if (!recipe || recipe->flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_LANES)) return -AFX_BAD_COMMAND;
    return afx_external_flow_upload(recipe, out);
}

int afx_instance_activate(afx_asset_t asset, afx_instance_t *out) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t asset_index;
    if (!out || !resolve_asset(asset, &asset_index) || !g_assets[asset_index].flow)
        return -AFX_INVALID_HANDLE;
    afx_asset_slot_t *flow = &g_assets[asset_index];
    uint8_t count = (uint8_t)flow->header.required_channels;
    if (flow->peak_commands > AFX_EXECUTION_BUDGET_COMMANDS - g_reserved_peak_commands ||
        flow->peak_register_writes > AFX_EXECUTION_BUDGET_WRITES - g_reserved_peak_writes)
        return -AFX_NO_EXEC_BUDGET;
    uint32_t index = 0;
    while (index < AFX_MAX_FLOW_SLOTS && (g_instances[index].live || g_instances[index].retired)) ++index;
    if (index == AFX_MAX_FLOW_SLOTS) return -AFX_NO_FLOW_SLOTS;
    uint64_t channels = allocate_channels(count);
    if (!channels) return -AFX_NO_CHANNELS;
    uint32_t map_address; uint8_t arena, offset;
    if (!map_allocate(count, channels, &map_address, &arena, &offset)) {
        g_available_channels |= channels;
        return -AFX_NO_FLOW_SLOTS;
    }
    afx_instance_slot_t *slot = &g_instances[index];
    if (!slot->generation) slot->generation = 1;
    afx_instance_t reference = AFX_MAKE_HANDLE(index, slot->generation);
    /* ARM deadlines are absolute. A one-tick lead admits queue/G2 latency while
     * preserving the authored deltas instead of making later flows catch up. */
    uint32_t start_tick = read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, timer_ticks)) + 1u;
    afx_activation_t activation = {
        .image_base = flow->addr, .image_size = flow->size,
        .stream_offset = flow->header.stream_offset, .stream_size = flow->header.stream_size,
        .setups_offset = flow->header.setups_offset, .setup_count = flow->header.setup_count,
        .channel_map = map_address, .required_channels = count,
        .flags = flow->header.flags & (AFX_FLAG_CONTROLLED | AFX_FLAG_MUSIC |
                                       AFX_FLAG_MUSIC_CHORUS | AFX_FLAG_LANES),
        .start_tick = start_tick
    };
    uint32_t sequence = new_sequence();
    int queued = enqueue(AFX_CMD_ACTIVATE, reference, sequence, 0, &activation, sizeof(activation));
    if (queued) {
        afx_instance_slot_t rollback = { .required_channels = count, .map_addr = map_address,
            .map_arena = arena, .map_offset = offset };
        map_release(&rollback);
        g_available_channels |= channels;
        return queued;
    }
    slot->live = true;
    slot->pending = true;
    slot->asset_index = asset_index;
    slot->map_addr = map_address;
    slot->required_channels = count;
    slot->map_arena = arena;
    slot->map_offset = offset;
    slot->channel_mask = channels;
    slot->start_tick = start_tick;
    slot->status = (afx_instance_status_t){ .reference = reference, .state = AFX_FREE,
                                             .sequence = sequence, .result = AFX_OK };
    ++flow->references;
    g_reserved_peak_commands += flow->peak_commands;
    g_reserved_peak_writes += flow->peak_register_writes;
    slot->work_reserved = true;
    *out = reference;
    return AFX_OK;
}
static int lifecycle(afx_instance_t instance, uint32_t opcode) {
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    afx_instance_slot_t *slot = &g_instances[index];
    if (slot->pending) return -AFX_BUSY;
    uint32_t sequence = new_sequence();
    int queued = enqueue(opcode, instance, sequence, 0, NULL, 0);
    if (!queued) {
        slot->pending = true;
        slot->recycling = opcode == AFX_CMD_RECYCLE;
        slot->status.sequence = sequence;
    }
    return queued;
}
int afx_instance_stop(afx_instance_t instance) {
    HOST_GUARD(-AFX_BUSY); return lifecycle(instance, AFX_CMD_STOP); }
int afx_instance_pause(afx_instance_t instance) {
    HOST_GUARD(-AFX_BUSY); return lifecycle(instance, AFX_CMD_PAUSE); }
int afx_instance_rebuild(afx_instance_t instance, const afx_restore_channel_t *states,
                         uint32_t state_count, uint32_t stream_position,
                         uint32_t local_tick, uint32_t next_deadline, bool run) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    afx_instance_slot_t *slot = &g_instances[index];
    if (slot->pending || slot->status.state != AFX_PAUSED || state_count > slot->required_channels ||
        (state_count && !states)) return -AFX_BAD_COMMAND;
    afx_asset_slot_t *flow = &g_assets[slot->asset_index];
    if (flow->sfx) return -AFX_UNSUPPORTED; /* Use bound NOTE setups, not raw restore addresses. */
    if (stream_position < flow->header.stream_offset ||
        stream_position >= flow->header.stream_offset + flow->header.stream_size) return -AFX_BAD_BOUNDS;
    uint32_t seen_low = 0, seen_high = 0;
    for (uint32_t i = 0; i < state_count; ++i) {
        uint32_t local = states[i].local_channel;
        uint32_t bit = 1u << (local & 31u);
        uint32_t *seen = local < 32 ? &seen_low : &seen_high;
        if (local >= slot->required_channels || (*seen & bit)) return -AFX_BAD_COMMAND;
        *seen |= bit;
    }
    uint32_t staging = 0;
    if (state_count) {
        staging = afx_mem_alloc(state_count * sizeof(*states), AFX_UPLOAD_ALIGN);
        if (!staging) return -AFX_NO_AICA_RAM;
        if (afx_mem_upload(staging, states, state_count * sizeof(*states))) {
            (void)afx_mem_free(staging);
            return -AFX_BAD_BOUNDS;
        }
    }
    afx_rebuild_payload_t request = { .states_address = staging, .state_count = state_count,
        .stream_position = stream_position, .local_tick = local_tick, .next_deadline = next_deadline };
    uint32_t sequence = new_sequence();
    int queued = enqueue(AFX_CMD_REBUILD, instance, sequence,
                         run ? AFX_REBUILD_RUN : 0, &request, sizeof(request));
    if (queued) {
        if (staging) (void)afx_mem_free(staging);
        return queued;
    }
    slot->pending = true;
    slot->staging_addr = staging;
    slot->status.sequence = sequence;
    return AFX_OK;
}
static int checkpoint_states(const afx_asset_slot_t *flow, uint32_t tick,
                             afx_restore_channel_t **states_out, uint32_t *count_out,
                             uint32_t *checkpoint_tick_out, uint32_t *position_out,
                             uint32_t *remaining_out) {
    const uint8_t *data = flow->checkpoints;
    uint32_t size = flow->checkpoints_size;
    if (!data || size < 16) return -AFX_UNSUPPORTED;
    if (afx_read32(data) != AFX_CHECKPOINT_MAGIC) return -AFX_UNSUPPORTED;
    if (afx_read32(data + 4) != AFX_CHECKPOINT_VERSION || afx_read32(data + 12)) return -AFX_BAD_FORMAT;
    uint32_t count = afx_read32(data + 8), cursor = 16, selected = UINT32_MAX, previous = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (cursor > size || size - cursor < 16) return -AFX_BAD_FORMAT;
        uint32_t at = afx_read32(data + cursor), states = afx_read32(data + cursor + 12);
        if ((i && at < previous) || states > flow->header.required_channels ||
            states > (size - cursor - 16) / sizeof(afx_restore_channel_t)) return -AFX_BAD_FORMAT;
        if (at <= tick) selected = cursor;
        previous = at;
        cursor += 16 + states * sizeof(afx_restore_channel_t);
    }
    if (cursor != size || selected == UINT32_MAX) return -AFX_BAD_FORMAT;
    uint32_t position = afx_read32(data + selected + 4);
    uint32_t remaining = afx_read32(data + selected + 8);
    uint32_t states = afx_read32(data + selected + 12);
    if (position < flow->header.stream_offset ||
        position >= flow->header.stream_offset + flow->header.stream_size) return -AFX_BAD_FORMAT;
    afx_restore_channel_t *out = states ? malloc(states * sizeof(*out)) : NULL;
    if (states && !out) return -AFX_NO_HOST_RAM;
    uint32_t seen_low = 0, seen_high = 0, source = selected + 16;
    for (uint32_t i = 0; i < states; ++i, source += sizeof(*out)) {
        uint32_t local = afx_read32(data + source);
        uint32_t bit = 1u << (local & 31u);
        uint32_t *seen = local < 32 ? &seen_low : &seen_high;
        if (local >= flow->header.required_channels || (*seen & bit)) { free(out); return -AFX_BAD_FORMAT; }
        *seen |= bit;
        out[i].local_channel = local;
        for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field)
            out[i].fields[field] = afx_read16(data + source + 4 + field * 2);
        if (!(out[i].fields[AFX_FIELD_CONTROL] & 0x400u)) {
            uint32_t relative = ((out[i].fields[AFX_FIELD_CONTROL] & 0x7fu) << 16) |
                                out[i].fields[AFX_FIELD_SAMPLE_LOW];
            if (relative >= flow->size || flow->addr > AFX_ASSET_LIMIT - relative) {
                free(out); return -AFX_BAD_FORMAT;
            }
            uint32_t address = flow->addr + relative;
            out[i].fields[AFX_FIELD_CONTROL] = (out[i].fields[AFX_FIELD_CONTROL] & ~0x7fu) |
                                                   ((address >> 16) & 0x7fu);
            out[i].fields[AFX_FIELD_SAMPLE_LOW] = address;
        }
    }
    *states_out = out; *count_out = states; *checkpoint_tick_out = afx_read32(data + selected);
    *position_out = position; *remaining_out = remaining;
    return AFX_OK;
}
static int flow_read(const afx_asset_slot_t *flow, uint32_t offset, void *data, uint32_t size) {
    if (offset > flow->size || size > flow->size - offset) return -AFX_BAD_BOUNDS;
    if (flow->image) { memcpy(data, flow->image + offset, size); return AFX_OK; }
    uint8_t *out = data;
    while (size) {
        uint32_t address = flow->addr + offset, skip = address & 3u;
        uint32_t word = g2_read_32(spu_base + (address & ~3u));
        uint32_t count = size < 4u - skip ? size : 4u - skip;
        for (uint32_t i = 0; i < count; ++i) out[i] = (uint8_t)(word >> (8u * (skip + i)));
        out += count;
        offset += count;
        size -= count;
    }
    return AFX_OK;
}
static int replay_checkpoint(const afx_asset_slot_t *flow, uint32_t target,
                             uint32_t checkpoint_tick, uint32_t position, uint32_t remaining,
                             afx_restore_channel_t **states_out, uint32_t *count_out,
                             uint32_t *position_out, uint32_t *remaining_out) {
    if (position >= flow->size || target < checkpoint_tick) return -AFX_BAD_FORMAT;
    afx_restore_channel_t active[AFX_MAX_FLOW_CHANNELS];
    uint8_t present[AFX_MAX_FLOW_CHANNELS] = {0};
    uint32_t count = *count_out;
    if (count > flow->header.required_channels) return -AFX_BAD_FORMAT;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t local = (*states_out)[i].local_channel;
        if (local >= flow->header.required_channels || present[local]) return -AFX_BAD_FORMAT;
        active[local] = (*states_out)[i]; present[local] = 1;
    }
    if (remaining > UINT32_MAX - checkpoint_tick) return -AFX_BAD_BOUNDS;
    uint32_t cursor = position, when = checkpoint_tick + remaining;
    while (cursor < flow->header.stream_offset + flow->header.stream_size) {
        uint8_t event_data[8 + AFX_SETUP_BYTES];
        uint32_t available = flow->header.stream_offset + flow->header.stream_size - cursor;
        if (available > sizeof(event_data)) available = sizeof(event_data);
        int read = flow_read(flow, cursor, event_data, available);
        if (read) return read;
        afx_event_t event;
        afx_result_t result = afx_decode_event(event_data, available, &event);
        if (result) return -(int)result;
        cursor += event.bytes;
        if (event.opcode == AFX_OP_WAIT8 || event.opcode == AFX_OP_WAIT16 || event.opcode == AFX_OP_WAIT32) {
            if (event.wait > UINT32_MAX - when) return -AFX_BAD_BOUNDS;
            when += event.wait;
            continue;
        }
        if (when > target) { cursor -= event.bytes; break; }
        if (event.opcode == AFX_OP_NOTE) {
            uint8_t setup[AFX_SETUP_BYTES];
            if (event.channel >= flow->header.required_channels || event.setup >= flow->header.setup_count ||
                flow_read(flow, flow->header.setups_offset + event.setup * AFX_SETUP_BYTES,
                          setup, sizeof(setup)) ||
                afx_apply_setup_fields(active[event.channel].fields, setup, event.mask, event.values,
                                       afx_field_value_bytes(event.mask))) return -AFX_BAD_FORMAT;
            active[event.channel].local_channel = event.channel;
            present[event.channel] = 1;
        } else if (event.opcode == AFX_OP_KEYOFF) {
            if (event.channel >= flow->header.required_channels) return -AFX_BAD_FORMAT;
            present[event.channel] = 0;
        } else if (event.opcode == AFX_OP_PATCH) {
            if (event.channel >= flow->header.required_channels) return -AFX_BAD_FORMAT;
            /* KEYOFF voices are omitted by checkpoint_plan. Updates to their
             * release tails remain legal, but must not resurrect a voice. */
            if (present[event.channel] &&
                afx_apply_fields(active[event.channel].fields, NULL, event.mask, event.values,
                    afx_field_value_bytes(event.mask))) return -AFX_BAD_FORMAT;
        } else if (event.opcode == AFX_OP_END || event.opcode == AFX_OP_PARK) {
            return -AFX_BAD_BOUNDS;
        } else return -AFX_BAD_FORMAT;
    }
    if (cursor >= flow->header.stream_offset + flow->header.stream_size || when <= target)
        return -AFX_BAD_BOUNDS;
    afx_restore_channel_t *out = NULL;
    uint32_t rebuilt = 0;
    for (uint32_t local = 0; local < flow->header.required_channels; ++local)
        if (present[local]) ++rebuilt;
    if (rebuilt) {
        out = malloc(rebuilt * sizeof(*out));
        if (!out) return -AFX_NO_HOST_RAM;
        uint32_t index = 0;
        for (uint32_t local = 0; local < flow->header.required_channels; ++local)
            if (present[local]) out[index++] = active[local];
    }
    free(*states_out);
    *states_out = out; *count_out = rebuilt; *position_out = cursor;
    *remaining_out = when - target;
    return AFX_OK;
}
int afx_instance_seek(afx_instance_t instance, uint32_t tick) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    afx_instance_slot_t *slot = &g_instances[index];
    if (slot->pending || slot->status.state != AFX_PAUSED) return -AFX_BUSY;
    afx_restore_channel_t *states;
    uint32_t checkpoint_tick, count, position, remaining;
    int result = checkpoint_states(&g_assets[slot->asset_index], tick, &states, &count,
                                   &checkpoint_tick, &position, &remaining);
    if (result) return result;
    result = replay_checkpoint(&g_assets[slot->asset_index], tick, checkpoint_tick, position,
                               remaining, &states, &count, &position, &remaining);
    if (result) { free(states); return result; }
    result = afx_instance_rebuild(instance, states, count, position, remaining,
                                  afx_status_timer_ticks() + 1u, true);
    free(states);
    return result;
}
int afx_instance_patch(afx_instance_t instance, uint8_t local_channel,
                       uint32_t mask, const uint16_t *values) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    if (g_instances[index].pending && g_instances[index].status.state != AFX_FREE)
        return -AFX_BUSY;
    if (local_channel >= g_instances[index].required_channels || (mask & ~AFX_FIELD_MASK))
        return -AFX_BAD_COMMAND;
    if (g_assets[g_instances[index].asset_index].sfx && (mask & 0x0fu))
        return -AFX_BAD_COMMAND; /* Sample bindings and bounds cannot change via IPC. */
    uint32_t count = afx_field_value_bytes(mask) / 2u;
    if (count && !values) return -AFX_BAD_BOUNDS;
    afx_patch_payload_t patch = { .local_channel = local_channel, .mask = mask };
    for (uint32_t i = 0; i < count; ++i) patch.values[i] = values[i];
    return enqueue(AFX_CMD_PATCH, instance, new_sequence(), 0, &patch, sizeof(patch));
}
int afx_instance_gain(afx_instance_t instance, uint8_t gain) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    if (g_instances[index].pending && g_instances[index].status.state != AFX_FREE)
        return -AFX_BUSY;
    afx_gain_payload_t request = { .gain = gain };
    return enqueue(AFX_CMD_INSTANCE_GAIN, instance, new_sequence(), 0, &request, sizeof(request));
}
int afx_instance_tempo(afx_instance_t instance, uint16_t scale_q8_8) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    if (g_instances[index].pending && g_instances[index].status.state != AFX_FREE)
        return -AFX_BUSY;
    if (scale_q8_8 < 16u || scale_q8_8 > 4096u) return -AFX_BAD_COMMAND;
    afx_tempo_payload_t request = {
        .period_q8_8 = (65536u + scale_q8_8 / 2u) / scale_q8_8
    };
    return enqueue(AFX_CMD_INSTANCE_TEMPO, instance, new_sequence(), 0,
                   &request, sizeof(request));
}
int afx_instance_lanes_set(afx_instance_t instance, uint32_t modifier,
                           uint8_t first_lane, uint32_t mask, const uint8_t values[32]) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    const afx_instance_slot_t *slot = &g_instances[index];
    const afx_asset_slot_t *flow = &g_assets[slot->asset_index];
    if (slot->pending && slot->status.state != AFX_FREE) return -AFX_BUSY;
    if (!(flow->header.flags & AFX_FLAG_LANES) || modifier >= AFX_LANE_MODIFIER_COUNT ||
        first_lane > AFX_MAX_FLOW_CHANNELS - 32u || !mask || !values) return -AFX_BAD_COMMAND;
    afx_lane_payload_t request = { .first_lane = first_lane, .mask = mask };
    memcpy(request.values, values, sizeof(request.values));
    return enqueue(AFX_CMD_LANE_SET, instance, new_sequence(), modifier, &request, sizeof(request));
}
static uint16_t dsp_word(const uint8_t *program, uint32_t offset) {
    return program[offset] | (uint16_t)program[offset + 1u] << 8;
}
static int scene_command(uint32_t opcode, uint32_t flags) {
    uint32_t sequence = new_sequence();
    int result = enqueue(opcode, AFX_DSP_SCENE_REFERENCE, sequence, flags, NULL, 0);
    if (result) return result;
    for (unsigned waited = 0; waited < 2000; ++waited) {
        if (read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, dsp_sequence)) == sequence) {
            uint32_t status = read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, dsp_result));
            return status ? -(int)status : AFX_OK;
        }
        thd_sleep(1);
    }
    return -AFX_TIMEOUT;
}
int afx_dsp_scene_enable(void) {
    HOST_GUARD(-AFX_BUSY);
    if (g_dsp_scene) return -AFX_BUSY;
    int result = scene_command(AFX_CMD_DSP_ENABLE, 0);
    if (!result) g_dsp_scene = true;
    return result;
}
int afx_dsp_scene_prepare(void) {
    HOST_GUARD(-AFX_BUSY);
    if (g_dsp_scene) return -AFX_BUSY;
    int result = scene_command(AFX_CMD_DSP_ENABLE, 1);
    if (!result) g_dsp_scene = true;
    return result;
}
int afx_dsp_scene_program(const void *data, uint32_t bytes) {
    HOST_GUARD(-AFX_BUSY);
    const uint8_t *program = data;
    if (!program || bytes != AFX_DSP_PROGRAM_BYTES || g_dsp_scene) return -AFX_BAD_COMMAND;
    int memory_format = -1;
    for (uint32_t step = 0; step < 128; ++step) {
        uint16_t w2 = dsp_word(program, step * 8u + 4u);
        if ((w2 & 0x8000u) || (!(step & 1u) && (w2 & 0x6000u)) ||
            ((w2 & 0x1000u) && ((w2 >> 8) & 15u) > 1u)) return -AFX_BAD_COMMAND;
        if (w2 & 0x6000u) {
            int nofl = (dsp_word(program, step * 8u + 6u) >> 15) & 1u;
            if (memory_format >= 0 && memory_format != nofl) return -AFX_BAD_COMMAND;
            memory_format = nofl;
        }
    }
    if ((dsp_word(program, 1408) | dsp_word(program, 1410)) & ~0x0f1fu)
        return -AFX_BAD_COMMAND;
    int result = scene_command(AFX_CMD_DSP_ENABLE, 1);
    if (result) return result;
    g_dsp_scene = true;
    g2_write_32(0xa0702000u, 0);
    g2_write_32(0xa0702004u, 0);
    /* Remove all memory/output writes before replacing their operands. */
    for (uint32_t i = 0; i < 128; ++i) g2_write_32(0xa0703408u + i * 16u, 2);
    thd_sleep(2);
    if (memory_format == 0)
        for (uint32_t i = 0; i < AFX_DSP_BYTES; i += 4)
            g2_write_32(spu_base + AFX_DSP_BASE + i, 0x60006000u);
    for (uint32_t i = 0; i < 128; ++i)
        g2_write_32(0xa0703000u + i * 4u, dsp_word(program, 1024u + i * 2u));
    for (uint32_t i = 0; i < 64; ++i)
        g2_write_32(0xa0703200u + i * 4u, dsp_word(program, 1280u + i * 2u));
    for (uint32_t i = 0; i < 512; ++i)
        g2_write_32(0xa0703400u + i * 4u, dsp_word(program, i * 2u));
    for (uint32_t i = 0; i < 512; ++i)
        if ((g2_read_32(0xa0703400u + i * 4u) & 0xffffu) != dsp_word(program, i * 2u))
            return -AFX_BAD_COMMAND;
    thd_sleep(2);
    g_dsp_return_left = dsp_word(program, 1408);
    g_dsp_return_right = dsp_word(program, 1410);
    g2_write_32(0xa0702000u, g_dsp_return_left);
    g2_write_32(0xa0702004u, g_dsp_return_right);
    return AFX_OK;
}
int afx_dsp_scene_returns(bool enabled) {
    HOST_GUARD(-AFX_BUSY);
    if (!g_dsp_scene) return -AFX_BUSY;
    g2_write_32(0xa0702000u, enabled ? g_dsp_return_left : 0);
    g2_write_32(0xa0702004u, enabled ? g_dsp_return_right : 0);
    return AFX_OK;
}
int afx_dsp_scene_disable(void) {
    HOST_GUARD(-AFX_BUSY);
    if (!g_dsp_scene) return -AFX_BUSY;
    int result = scene_command(AFX_CMD_DSP_DISABLE, 0);
    if (!result) {
        g_dsp_scene = false;
        g_dsp_return_left = g_dsp_return_right = 0;
    }
    return result;
}
int afx_instance_recycle(afx_instance_t instance) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    if (g_instances[index].pending || (g_instances[index].status.state != AFX_DONE &&
                                       g_instances[index].status.state != AFX_ERROR))
        return -AFX_BUSY;
    return lifecycle(instance, AFX_CMD_RECYCLE);
}
int afx_update(void) {
    HOST_GUARD(-AFX_BUSY);
    if (!g_ready || !g_lifecycle) return -AFX_UNSUPPORTED;
    for (uint32_t index = 0; index < AFX_MAX_FLOW_SLOTS; ++index) {
        afx_instance_slot_t *slot = &g_instances[index];
        if (!slot->live) continue;
        afx_instance_status_t observed;
        if (!read_observed(index, &observed)) continue;
        afx_instance_t reference = AFX_MAKE_HANDLE(index, slot->generation);
        /* FREE carries no generation in its reference, so its sequence is the
         * guard against a delayed recycle observation freeing this slot after
         * it has been reused by a later generation. */
        if (slot->recycling && observed.reference == 0 && observed.state == AFX_FREE &&
            observed.sequence == slot->status.sequence) {
            release_instance_preserving_generation(index);
            continue;
        }
        if (observed.reference != reference || !observed.sequence ||
            observed.sequence < slot->status.sequence) continue;
        /* Bound SFX cannot rebuild; PARK permanently ends their stream work.
         * Keep channel/sample ownership and the independent IPC limits intact. */
        if (observed.state == AFX_PARKED && g_assets[slot->asset_index].sfx)
            release_instance_work(slot);
        uint32_t expected = slot->status.sequence;
        if (memcmp(&observed, &slot->status, sizeof(observed)))
            slot->status = observed;
        if (slot->pending && observed.sequence >= expected)
            {
                if (slot->staging_addr) {
                    (void)free_allocation(slot->staging_addr);
                    slot->staging_addr = 0;
                }
                slot->pending = false;
            }
    }
    return AFX_OK;
}
int afx_instance_status(afx_instance_t instance, afx_instance_status_t *out) {
    HOST_GUARD(-AFX_BUSY);
    uint32_t index;
    if (!out || !resolve_instance(instance, &index)) return -AFX_STALE_GENERATION;
    *out = g_instances[index].status;
    return AFX_OK;
}
uint32_t afx_instance_start_tick(afx_instance_t instance) {
    HOST_GUARD(0);
    uint32_t index;
    return resolve_instance(instance, &index) ? g_instances[index].start_tick : 0;
}

/* ABI-1 compatibility symbols remain intentionally inert. */
int afx_push_cmd(uint32_t cmd, uint32_t a, uint32_t b, uint32_t c) {
    (void)cmd; (void)a; (void)b; (void)c; return -AFX_UNSUPPORTED;
}
int afx_flow_activate(uint8_t slot, afx_asset_t asset) {
    (void)slot; (void)asset; return -AFX_UNSUPPORTED;
}
int afx_flow_play(uint8_t slot) { (void)slot; return -AFX_UNSUPPORTED; }
int afx_flow_stop(uint8_t slot) { (void)slot; return -AFX_UNSUPPORTED; }
int afx_flow_pause(uint8_t slot) { (void)slot; return -AFX_UNSUPPORTED; }
int afx_flow_resume(uint8_t slot) { (void)slot; return -AFX_UNSUPPORTED; }
int afx_flow_release_completed(uint8_t slot) { (void)slot; return -AFX_UNSUPPORTED; }
int afx_flow_set_channel_reg16(uint8_t slot, uint8_t ch, uint8_t reg, uint16_t value) {
    (void)slot; (void)ch; (void)reg; (void)value; return -AFX_UNSUPPORTED;
}
int afx_flow_set_channel_reg32(uint8_t slot, uint8_t ch, uint8_t reg, uint32_t value) {
    (void)slot; (void)ch; (void)reg; (void)value; return -AFX_UNSUPPORTED;
}
int afx_master_volume(uint8_t volume) { (void)volume; return -AFX_UNSUPPORTED; }
int afx_poll_events(uint32_t *mask) { if (mask) *mask = 0; return -AFX_UNSUPPORTED; }
volatile afx_status_t *afx_status(void) { return (void *)(uintptr_t)(spu_base + AFX_STATUS_ADDR); }
uint32_t afx_status_heartbeat(void) {
    return read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, heartbeat));
}
uint32_t afx_status_timer_ticks(void) {
    return read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, timer_ticks));
}
uint32_t afx_status_max_lateness(void) {
    return read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, max_lateness));
}
