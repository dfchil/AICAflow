#ifndef AICAFLOW_HOST_INTERNAL_H
#define AICAFLOW_HOST_INTERNAL_H

#include <aicaflow/host.h>
#include <aicaflow/codec.h>

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
    afx_file_header_t header;
} afx_asset_slot_t;
typedef struct {
    bool live, retired, pending, recycling, work_reserved;
    uint16_t generation;
    uint32_t asset_index, map_addr, staging_addr, start_tick;
    uint8_t required_channels, map_arena, map_offset;
    uint64_t channel_mask;
    afx_instance_status_t status;
} afx_instance_slot_t;

extern uint32_t g_dynamic_base;
extern afx_block_t *g_free_blocks, *g_allocs;
extern uint32_t g_free_count, g_free_capacity, g_alloc_count, g_alloc_capacity;
extern afx_asset_slot_t *g_assets;
extern uint32_t g_asset_capacity;
extern afx_instance_slot_t g_instances[AFX_MAX_FLOW_SLOTS];
extern uint64_t g_available_channels, g_map_used[AFX_CHANNEL_MAP_ARENAS];
extern uint32_t g_reserved_peak_commands, g_reserved_peak_writes, g_next_sequence;
extern bool g_dsp_scene, g_ready, g_lifecycle;
extern uint16_t g_dsp_return_left, g_dsp_return_right;
extern volatile bool g_upload_dma_done;
extern afx_asset_t g_upload_dma_asset;
extern uint32_t g_upload_dma_bytes, g_upload_dma_offset;
extern bool g_upload_dma_stream;
extern mutex_t g_host_mutex;
extern uint32_t const g_spu_base;

static inline void host_unlock(mutex_t **guard) { if (*guard) mutex_unlock(*guard); }
#define HOST_GUARD(failure) \
    mutex_t *host_guard __attribute__((cleanup(host_unlock))) = NULL; \
    if (mutex_trylock(&g_host_mutex)) return failure; \
    host_guard = &g_host_mutex

uint32_t align_up(uint32_t value, uint32_t align);
bool in_asset_arena(uint32_t address, uint32_t size);
bool allocation_diagnostic(uint32_t size, uint32_t align, afx_mem_diagnostic_t *out);
bool reserve_assets(uint32_t need);
bool allocator_reset(uint32_t dynamic_base);
bool free_allocation(uint32_t address);
bool resolve_asset(afx_asset_t asset, uint32_t *index);
afx_asset_t reserve_asset(uint32_t size, uint32_t align, bool live);

void upload_dma_wait(void);
void upload_dma_complete(void *ignored);
void *upload_image_alloc(uint32_t size);
uint32_t read_spu_word(uint32_t address);
void upload_words(uint32_t address, const void *data, uint32_t size);

void maps_reset(void);
bool resolve_instance(afx_instance_t instance, uint32_t *index);
void release_instance_work(afx_instance_slot_t *slot);
void release_instance_preserving_generation(uint32_t index);

uint32_t new_sequence(void);
int enqueue(uint32_t opcode, uint32_t reference, uint32_t sequence,
            uint32_t flags, const void *payload, uint32_t payload_size);
bool read_observed(uint32_t index, afx_instance_status_t *out);

#endif
