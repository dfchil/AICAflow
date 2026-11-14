#include "host_internal.h"
#include <aicaflow/scene.h>

typedef struct {
    uint32_t kind; /* AFX_SCENE_* kind of the entry. */
    uint32_t name; /* Offset of its name in the scene's names; zero for none. */
    uint32_t offset; /* Byte offset of its range in the block. */
    uint32_t bytes; /* Size of that range. */
    uint32_t ticks; /* A flow's authored length in ticks. */
    afx_asset_t flow; /* A flow entry's flow, or a slot's loaded flow. */
    afx_bank_t bank; /* A bank entry's bank, or a slot's loaded bank. */
} scene_entry_t;

typedef struct {
    bool live; /* The program holds the handle. */
    bool released; /* Given up: the block is freed with the last asset in it. */
    bool retired; /* The generation would wrap; the slot is not reused. */
    uint16_t generation; /* Generation encoded in the scene handle. */
    uint32_t block; /* AICA address of the scene's block; zero for a free slot. */
    uint32_t block_bytes; /* Size of the block. */
    uint32_t assets; /* Banks and flows still placed in the block. */
    uint32_t entry_count; /* Entries of the AFS file, in its order. */
    scene_entry_t *entries;
    char *names; /* The entries' names, each zero-terminated; names[0] is "". */
    uint32_t parent_count; /* Scenes whose banks the flows may bind besides this one's. */
    afx_scene_t *parents;
} scene_slot_t;

static scene_slot_t g_scenes[AFX_MAX_SCENES];

static void slot_clear(scene_slot_t *s) {
    uint16_t generation = s->generation;
    bool retired = s->retired;
    free(s->entries);
    free(s->names);
    free(s->parents);
    memset(s, 0, sizeof(*s));
    s->generation = generation;
    s->retired = retired;
}
static void retire_handle(scene_slot_t *s) {
    if (s->generation == UINT16_MAX) s->retired = true;
    else ++s->generation;
}
void scenes_reset(void) {
    for (uint32_t i = 0; i < AFX_MAX_SCENES; ++i) {
        if (g_scenes[i].live || g_scenes[i].block) retire_handle(&g_scenes[i]);
        slot_clear(&g_scenes[i]);
    }
}
static scene_slot_t *resolve_scene(afx_scene_t scene) {
    uint32_t raw = scene & 0xffffu;
    if (!raw || raw > AFX_MAX_SCENES) return NULL;
    scene_slot_t *s = &g_scenes[raw - 1u];
    return s->live && s->generation == AFX_HANDLE_GENERATION(scene) ? s : NULL;
}
static scene_slot_t *scene_at(uint32_t address) {
    for (uint32_t i = 0; i < AFX_MAX_SCENES; ++i)
        if (g_scenes[i].block && address >= g_scenes[i].block &&
            address - g_scenes[i].block < g_scenes[i].block_bytes) return &g_scenes[i];
    return NULL;
}
bool scene_block(uint32_t address) {
    for (uint32_t i = 0; i < AFX_MAX_SCENES; ++i)
        if (address && g_scenes[i].block == address) return true;
    return false;
}
static void scene_free(scene_slot_t *s) {
    (void)free_allocation(s->block);
    slot_clear(s);
}
void scene_asset_freed(uint32_t address) {
    scene_slot_t *s = scene_at(address);
    if (s && s->assets && !--s->assets && s->released) scene_free(s);
}
uint32_t scene_ring(uint32_t bytes) {
    for (uint32_t i = 0; i < AFX_MAX_SCENES; ++i) {
        const scene_slot_t *s = &g_scenes[i];
        for (uint32_t j = 0; s->live && j < s->entry_count; ++j)
            if (s->entries[j].kind == AFX_SCENE_RING && s->entries[j].bytes >= bytes)
                return s->block + s->entries[j].offset;
    }
    return 0;
}

static scene_entry_t *entry_named(scene_slot_t *s, const char *name) {
    for (uint32_t i = 0; name && i < s->entry_count; ++i)
        if (s->entries[i].name && !strcmp(s->names + s->entries[i].name, name)) return &s->entries[i];
    return NULL;
}
static const afx_bank_t *scene_bank(const scene_slot_t *s, uint32_t low, uint32_t high) {
    for (uint32_t i = 0; i < s->entry_count; ++i) {
        const afx_bank_t *bank = &s->entries[i].bank;
        if (s->entries[i].kind == AFX_SCENE_BANK && bank->asset && bank->id.low == low &&
            bank->id.high == high) return bank;
    }
    return NULL;
}
/* The banks an AFX file names, slot by slot: own (a slot's bank) if it has the
 * identity, else the scene's bank with it, else a parent's. */
static int flow_banks(const scene_slot_t *s, const afx_bank_t *own, const void *data, uint32_t size,
                      const afx_bank_t *banks[AFX_MAX_FLOW_BANKS], uint32_t *count) {
    afx_file_header_t header;
    int valid = flow_bank_header(data, size, &header);
    if (valid) return valid;
    *count = afx_file_bank_count(data, &header);
    for (uint32_t slot = 0; slot < *count; ++slot) {
        uint32_t low, high;
        afx_file_bank_identity(data, &header, slot, &low, &high);
        const afx_bank_t *bank = own && own->asset && own->id.low == low && own->id.high == high ?
                                 own : scene_bank(s, low, high);
        for (uint32_t i = 0; !bank && i < s->parent_count; ++i) {
            const scene_slot_t *parent = resolve_scene(s->parents[i]);
            if (parent) bank = scene_bank(parent, low, high);
        }
        if (!bank) return -AFX_BAD_SAMPLE;
        banks[slot] = bank;
    }
    return AFX_OK;
}
/* Releases the scene's flows, then its banks: each is freed now if nothing
 * uses it, otherwise when the last instance or flow that does lets go. */
static void release_assets(scene_slot_t *s) {
    for (uint32_t pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < s->entry_count; ++i) {
            afx_asset_t asset = pass ? s->entries[i].bank.asset : s->entries[i].flow;
            uint32_t index;
            if (!asset || !resolve_asset(asset, &index)) continue;
            if (g_upload_dma_asset == asset) upload_dma_wait();
            if (g_assets[index].references) g_assets[index].release_wanted = true;
            else (void)free_asset_slot(index);
        }
}
/* Frees the block once nothing is placed in it any more. */
static void give_up(scene_slot_t *s) {
    release_assets(s);
    s->released = true;
    if (!s->assets) scene_free(s);
}

int afx_scene_load(const void *data, uint32_t size, const afx_scene_t *parents,
                   uint32_t parent_count, afx_scene_t *out) {
    HOST_GUARD(-AFX_BUSY);
    afx_scene_header_t header;
    if (!out) return -AFX_BAD_BOUNDS;
    *out = 0;
    if (!g_ready || (parent_count && !parents) || parent_count > AFX_MAX_SCENES) return -AFX_BAD_BOUNDS;
    afx_result_t valid = afx_scene_validate(data, size, &header);
    if (valid) return -(int)valid;
    for (uint32_t i = 0; i < parent_count; ++i)
        if (!resolve_scene(parents[i])) return -AFX_INVALID_HANDLE;
    uint32_t index = 0;
    while (index < AFX_MAX_SCENES &&
           (g_scenes[index].live || g_scenes[index].block || g_scenes[index].retired)) ++index;
    if (index == AFX_MAX_SCENES) return -AFX_NO_HOST_RAM;
    scene_slot_t *s = &g_scenes[index];
    if (!s->generation) s->generation = 1;

    /* A ring needs its 2 KiB boundary in AICA RAM, so the block is aligned so. */
    uint32_t name_bytes = 1, align = AFX_UPLOAD_ALIGN;
    for (uint32_t i = 0; i < header.entry_count; ++i) {
        afx_scene_entry_t e = afx_scene_entry_at(data, i);
        if (e.name) name_bytes += (uint32_t)strlen(afx_scene_entry_name(data, &e)) + 1u;
        if (e.kind == AFX_SCENE_RING) align = AFX_SCENE_RING_ALIGN;
    }
    s->entries = calloc(header.entry_count, sizeof(*s->entries));
    s->names = malloc(name_bytes);
    s->parents = parent_count ? malloc(parent_count * sizeof(*s->parents)) : NULL;
    if (!s->entries || !s->names || (parent_count && !s->parents)) {
        slot_clear(s);
        return -AFX_NO_HOST_RAM;
    }
    s->block = afx_mem_alloc(header.block_bytes, align);
    if (!s->block) {
        slot_clear(s);
        return -AFX_NO_AICA_RAM;
    }
    s->block_bytes = header.block_bytes;
    s->entry_count = header.entry_count;
    s->parent_count = parent_count;
    if (parent_count) memcpy(s->parents, parents, parent_count * sizeof(*s->parents));
    s->names[0] = 0;
    for (uint32_t i = 0, at = 1; i < header.entry_count; ++i) {
        afx_scene_entry_t e = afx_scene_entry_at(data, i);
        scene_entry_t *entry = &s->entries[i];
        entry->kind = e.kind;
        entry->offset = e.block_offset;
        entry->bytes = e.block_bytes;
        entry->ticks = e.ticks;
        if (e.name) {
            uint32_t bytes = (uint32_t)strlen(afx_scene_entry_name(data, &e)) + 1u;
            memcpy(s->names + at, afx_scene_entry_name(data, &e), bytes);
            entry->name = at;
            at += bytes;
        }
    }

    /* Banks first, so that the flows find them. */
    int result = AFX_OK;
    for (uint32_t pass = 0; pass < 2 && !result; ++pass)
        for (uint32_t i = 0; i < header.entry_count && !result; ++i) {
            afx_scene_entry_t e = afx_scene_entry_at(data, i);
            scene_entry_t *entry = &s->entries[i];
            const uint8_t *file = (const uint8_t *)data + e.file_offset;
            uint32_t place = s->block + e.block_offset;
            if (!pass && e.kind == AFX_SCENE_BANK) {
                /* Two banks with one identity would make the binding ambiguous. */
                if (scene_bank(s, afx_read32(file + 8), afx_read32(file + 12))) result = -AFX_BAD_FORMAT;
                else result = bank_upload(&entry->bank, file, e.file_size, place, e.block_bytes);
            } else if (pass && (e.kind == AFX_SCENE_FLOW || e.kind == AFX_SCENE_SONG)) {
                const afx_bank_t *banks[AFX_MAX_FLOW_BANKS];
                uint32_t count = 0;
                result = flow_banks(s, NULL, file, e.file_size, banks, &count);
                if (!result)
                    result = flow_upload(banks, count, file, e.file_size, place, e.block_bytes, &entry->flow);
            } else {
                continue;
            }
            if (!result) ++s->assets;
        }
    if (result) {
        give_up(s);
        return result;
    }
    s->live = true;
    *out = AFX_MAKE_HANDLE(index, s->generation);
    return AFX_OK;
}

int afx_scene_find(afx_scene_t scene, const char *name, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    scene_slot_t *s = resolve_scene(scene);
    if (!s) return -AFX_INVALID_HANDLE;
    const scene_entry_t *entry = entry_named(s, name);
    if (!entry || !entry->flow) return -AFX_INVALID_HANDLE;
    *out = entry->flow;
    return AFX_OK;
}

int afx_scene_entry(afx_scene_t scene, uint32_t index, afx_scene_info_t *out) {
    HOST_GUARD(-AFX_BUSY);
    scene_slot_t *s = resolve_scene(scene);
    if (!s) return -AFX_INVALID_HANDLE;
    if (!out || index >= s->entry_count) return -AFX_BAD_BOUNDS;
    const scene_entry_t *entry = &s->entries[index];
    uint32_t flow;
    *out = (afx_scene_info_t){
        .kind = entry->kind,
        .name = s->names + entry->name,
        .asset = entry->kind == AFX_SCENE_BANK ? entry->bank.asset : entry->flow,
        .addr = s->block + entry->offset,
        .bytes = entry->bytes,
        .ticks = entry->ticks,
        .loops = entry->flow && resolve_asset(entry->flow, &flow) &&
                 g_assets[flow].loop_index != AFX_SONG_NO_LOOP,
    };
    return AFX_OK;
}

int afx_scene_slot_load(afx_scene_t scene, const char *slot, const void *bank, uint32_t bank_size,
                        const void *flow, uint32_t flow_size, afx_asset_t *out) {
    HOST_GUARD(-AFX_BUSY);
    if (!out) return -AFX_BAD_BOUNDS;
    *out = AFX_ASSET_INVALID;
    scene_slot_t *s = resolve_scene(scene);
    scene_entry_t *entry = s ? entry_named(s, slot) : NULL;
    if (!entry || entry->kind != AFX_SCENE_SLOT) return -AFX_INVALID_HANDLE;
    if (!flow) return -AFX_BAD_BOUNDS;

    /* What the slot holds must be idle: no instance of its flow, and no flow
     * but its own bound to its bank. */
    uint32_t flow_index = 0, bank_index = 0, binds = 0;
    bool old_flow = entry->flow && resolve_asset(entry->flow, &flow_index);
    bool old_bank = entry->bank.asset && resolve_asset(entry->bank.asset, &bank_index);
    if (old_flow && g_assets[flow_index].references) return -AFX_ASSET_REFERENCED;
    for (uint32_t i = 0; old_flow && old_bank && i < AFX_MAX_FLOW_BANKS; ++i)
        binds += flow_bank(flow_index, i) == entry->bank.asset;
    if (old_bank && g_assets[bank_index].references != binds) return -AFX_ASSET_REFERENCED;
    if (old_flow) (void)free_asset_slot(flow_index);
    if (old_bank) (void)free_asset_slot(bank_index);
    entry->flow = AFX_ASSET_INVALID;
    entry->bank = (afx_bank_t){0};

    /* The bank's payload at the slot's start, the flow's image after it. */
    uint32_t place = s->block + entry->offset, at = 0;
    int result = AFX_OK;
    if (bank) {
        result = bank_upload(&entry->bank, bank, bank_size, place, entry->bytes);
        if (!result) {
            ++s->assets;
            at = align_up(entry->bank.bytes, AFX_UPLOAD_ALIGN);
        }
    }
    const afx_bank_t *banks[AFX_MAX_FLOW_BANKS];
    uint32_t count = 0;
    if (!result) result = flow_banks(s, &entry->bank, flow, flow_size, banks, &count);
    if (!result)
        result = flow_upload(banks, count, flow, flow_size, place + at, entry->bytes - at, &entry->flow);
    if (!result) {
        ++s->assets;
        *out = entry->flow;
        return AFX_OK;
    }
    if (entry->bank.asset) discard_asset(entry->bank.asset);
    entry->bank = (afx_bank_t){0};
    return result;
}

int afx_scene_release(afx_scene_t scene) {
    HOST_GUARD(-AFX_BUSY);
    scene_slot_t *s = resolve_scene(scene);
    if (!s) return -AFX_INVALID_HANDLE;
    if (g_dsp_ring_placed && scene_at(g_dsp_ring) == s) return -AFX_ASSET_REFERENCED;
    s->live = false;
    retire_handle(s);
    give_up(s);
    return AFX_OK;
}
