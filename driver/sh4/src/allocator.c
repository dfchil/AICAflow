#include "host_internal.h"

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
static bool insert_free_block(uint32_t address, uint32_t size);
bool reserve_assets(uint32_t need) {
    return need <= UINT16_MAX &&
           grow((void **)&g_assets, &g_asset_capacity, need, sizeof(*g_assets));
}
uint32_t align_up(uint32_t value, uint32_t align) {
    if (!align || (align & (align - 1u)) || value > UINT32_MAX - (align - 1u))
        return 0;
    return (value + align - 1u) & ~(align - 1u);
}
bool in_asset_arena(uint32_t address, uint32_t size) {
    return g_dynamic_base && size && address >= g_dynamic_base &&
           address < g_asset_limit && afx_range(address, size, g_asset_limit);
}
bool allocation_diagnostic(uint32_t size, uint32_t align, afx_mem_diagnostic_t *out) {
    if (!g_ready || !out || !size || (align && (align & (align - 1u)))) return false;
    uint32_t use_align = align > AFX_UPLOAD_ALIGN ? align : AFX_UPLOAD_ALIGN;
    uint32_t aligned = align_up(size, AFX_UPLOAD_ALIGN);
    if (!aligned || use_align > g_asset_limit) return false;
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
bool allocator_reset(uint32_t dynamic_base) {
    upload_dma_wait();
    g_dynamic_base = 0;
    g_asset_limit = AFX_ASSET_MAX;
    g_free_count = g_alloc_count = 0;
    for (uint32_t i = 0; i < g_asset_capacity; ++i) {
        free(g_assets[i].checkpoints);
        g_assets[i].bank = AFX_ASSET_INVALID;
        g_assets[i].sample_bank = false;
        g_assets[i].checkpoints = NULL;
        g_assets[i].checkpoints_size = 0;
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
    g_dsp_ring = 0;
    g_dsp_return_left = g_dsp_return_right = 0;
    g_next_sequence = 1;
    if (!dynamic_base || dynamic_base >= g_asset_limit || !reserve_free(1))
        return false;
    g_dynamic_base = dynamic_base;
    g_free_blocks[0] = (afx_block_t){dynamic_base, g_asset_limit - dynamic_base};
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
    if (!size || use_align > g_asset_limit || !reserve_allocs(g_alloc_count + 1))
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
bool free_allocation(uint32_t address) {
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
    if (address && address == g_dsp_ring) return -AFX_ASSET_REFERENCED;
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
            if (g_allocs[i].addr == g_dsp_ring) return -AFX_ASSET_REFERENCED;
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
    out->asset_limit = g_asset_limit;
    out->total_bytes = g_asset_limit - g_dynamic_base;
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
bool resolve_asset(afx_asset_t asset, uint32_t *index) {
    if (!asset || !index) return false;
    uint32_t raw = asset & 0xffffu;
    if (!raw || raw > g_asset_capacity) return false;
    afx_asset_slot_t *slot = &g_assets[raw - 1u];
    if ((!slot->live && !slot->uploading) || slot->generation != AFX_HANDLE_GENERATION(asset)) return false;
    *index = raw - 1u;
    return true;
}
afx_asset_t reserve_asset(uint32_t size, uint32_t align, bool live) {
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
    slot->bank = AFX_ASSET_INVALID;
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
    if (!free_allocation(slot->addr)) return -AFX_BAD_BOUNDS;
    if (slot->bank) {
        uint32_t bank_index;
        if (resolve_asset(slot->bank, &bank_index)) --g_assets[bank_index].references;
    }
    slot->bank = AFX_ASSET_INVALID;
    slot->sample_bank = false;
    free(slot->checkpoints);
    slot->checkpoints = NULL;
    slot->checkpoints_size = 0;
    slot->live = false;
    slot->flow = false;
    slot->uploading = false;
    slot->addr = slot->size = slot->allocation_size = 0;
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
