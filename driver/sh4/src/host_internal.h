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

typedef struct {
    uint32_t addr; /* Absolute AICA byte address of an allocator block. */
    uint32_t size; /* Block length in bytes. */
} afx_block_t;
typedef struct {
    bool live; /* Asset handle is available for use. */
    bool retired; /* Slot cannot be reused because its generation would wrap. */
    bool flow; /* Asset contains an executable AFX flow. */
    bool uploading; /* Upload is incomplete; asset cannot yet be activated. */
    bool sample_bank; /* Asset is a contiguous sample-bank payload. */
    uint16_t generation; /* Handle generation; advanced when a slot is released. */
    uint32_t addr; /* Absolute AICA byte address of asset data. */
    uint32_t size; /* Logical asset data length in bytes, excluding allocation padding. */
    uint32_t allocation_size; /* Backing allocation length in bytes, including padding. */
    uint32_t references; /* Outstanding instance/dependent-asset references preventing release. */
    uint32_t upload_cursor; /* Logical bytes uploaded so far. */
    uint8_t *checkpoints; /* Owned SH-4 copy of the optional checkpoint payload; never sent to ARM7. */
    uint32_t checkpoints_size; /* Checkpoint payload length in bytes. */
    uint32_t peak_commands; /* Peak authored command count between WAIT boundaries. */
    uint32_t peak_register_writes; /* Peak register-write budget between WAIT boundaries. */
    afx_asset_t bank; /* Sole retained sample-bank handle, or AFX_ASSET_INVALID. */
    uint32_t flags; /* Authored AFX_FLAG_* playback options. */
    uint32_t stream_offset; /* First instruction byte offset within the AICA image. */
    uint32_t stream_size; /* Instruction-stream length in bytes. */
    uint32_t setup_count; /* Number of setup templates at the image start. */
    uint32_t required_channels; /* Number of local voices required by this flow. */
    uint32_t control_id; /* Identity matched against the optional AFC sidecar. */
    uint32_t bank_id_low; /* Low half of the authored bank identity. */
    uint32_t bank_id_high; /* High half of the authored bank identity. */
} afx_asset_slot_t;
#if defined(__sh__)
_Static_assert(sizeof(afx_asset_slot_t) == 80, "SH-4 asset bookkeeping size");
#endif
typedef struct {
    bool live; /* Instance handle is allocated. */
    bool retired; /* Slot is permanently retired to prevent generation wrap. */
    bool pending; /* Waiting for a lifecycle/rebuild command observation. */
    bool recycling; /* Waiting for RECYCLE acknowledgement before releasing the slot. */
    bool work_reserved; /* Instance currently contributes to reserved playback work budgets. */
    uint16_t generation; /* Generation encoded in the instance handle. */
    uint32_t asset_index; /* Index of the retained flow in g_assets. */
    uint32_t map_addr; /* Absolute AICA byte address of the uploaded channel map. */
    uint32_t staging_addr; /* Temporary AICA restore-state allocation; zero when absent. */
    uint32_t start_tick; /* Initial scheduled start time in hardware timer ticks. */
    uint8_t required_channels; /* Number of local channels owned by this instance. */
    uint8_t map_arena; /* Index of the channel-map arena containing this map. */
    uint8_t map_offset; /* Byte offset within that channel-map arena. */
    uint64_t channel_mask; /* Bit n marks physical AICA channel n owned by this instance. */
    afx_instance_status_t status; /* Latest accepted ARM7 observation, cached on SH-4. */
} afx_instance_slot_t;

extern uint32_t g_dynamic_base, g_asset_limit;
extern afx_block_t *g_free_blocks, *g_allocs;
extern uint32_t g_free_count, g_free_capacity, g_alloc_count, g_alloc_capacity;
extern afx_asset_slot_t *g_assets;
extern uint32_t g_asset_capacity;
extern afx_instance_slot_t g_instances[AFX_MAX_FLOW_SLOTS];
extern uint64_t g_available_channels, g_map_used[AFX_CHANNEL_MAP_ARENAS];
extern uint32_t g_reserved_peak_commands, g_reserved_peak_writes, g_next_sequence;
extern bool g_dsp_scene, g_ready, g_lifecycle;
extern uint32_t g_dsp_ring; /* Owned DSP allocation; retained until stop is acknowledged. */
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
