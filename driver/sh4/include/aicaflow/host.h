#ifndef AICAFLOW_HOST_H
#define AICAFLOW_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <aicaflow/protocol.h>
#include <aicaflow/dsp.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AFX_ASSET_INVALID 0u

typedef uint32_t afx_asset_t;
typedef uint32_t afx_instance_t;

typedef struct {
    uint32_t reference; /* Generation-bearing instance handle from ARM7. */
    uint32_t state; /* Current observed AFX_* playback state. */
    uint32_t sequence; /* Sequence associated with the latest accepted observation. */
    uint32_t result; /* Nonnegative ARM7 afx_result_t, unlike negative host API failures. */
    uint32_t position; /* Instruction byte offset relative to the uploaded AFX image. */
    uint32_t next_deadline; /* Next event deadline in wrapping hardware timer ticks. */
    uint32_t detail; /* State-specific diagnostic: lateness, channel or error detail. */
} afx_instance_status_t;

typedef struct {
    uint32_t total_bytes; /* Asset arena size in bytes, including space used by DSP allocations. */
    uint32_t used_bytes; /* Allocated bytes in the asset arena. */
    uint32_t free_bytes; /* Total unallocated bytes in the asset arena. */
    uint32_t largest_free_block; /* Largest contiguous free range in bytes. */
    uint32_t free_block_count; /* Number of free ranges tracked by the allocator. */
    uint32_t active_allocations; /* Number of live AICA allocations. */
    uint32_t dynamic_base; /* First usable AICA asset byte address. */
    uint32_t asset_limit; /* Exclusive current AICA asset ceiling. */
} afx_mem_stats_t;
enum {
    AFX_MEM_AVAILABLE,
    AFX_MEM_FRAGMENTED,
    AFX_MEM_EXHAUSTED
};
typedef struct {
    uint32_t requested_bytes; /* Caller-requested allocation size in bytes. */
    uint32_t aligned_bytes; /* Requested size rounded to the 32-byte upload granularity. */
    uint32_t free_bytes; /* Total currently free asset bytes. */
    uint32_t largest_free_block; /* Largest free range in bytes, before start-address alignment. */
    uint32_t result; /* AFX_MEM_AVAILABLE, AFX_MEM_FRAGMENTED or AFX_MEM_EXHAUSTED. */
} afx_mem_diagnostic_t;
typedef struct {
    uint32_t peak_commands; /* Peak authored commands between WAIT boundaries. */
    uint32_t peak_register_writes; /* Peak register writes between WAIT boundaries. */
} afx_work_profile_t;

/* One caller-selected upload step is capped so gameplay can interleave normal
 * host work. The caller drives it; no hidden worker or retry queue exists. */
#define AFX_UPLOAD_STEP_BYTES 4096u
/* Queues one 32-byte-aligned SH-4 -> AICA DMA transfer. Call again on later
 * frames to collect completion and queue the next block. Every byte, including
 * final zero padding, uses DMA; 64 KiB keeps an interactive frame between blocks. */
#define AFX_UPLOAD_DMA_STEP_BYTES 65536u
/* Init/shutdown require all other host calls to be quiescent. Other host APIs
 * serialize bookkeeping and publication with a recursive try-lock. Every
 * operation that can fail returns 0 on success and a negative afx_result_t on
 * failure. Handle/address getters use 0 only for an invalid query result.
 * Upload and seek hold the lock for their complete call; use upload_step to
 * bound transfer size. Status timer/heartbeat reads are independent G2 reads.
 * Integer result APIs return 0 or negative afx_result_t; firmware must match. */
int afx_init(const void *arm7_firmware, uint32_t firmware_size);
void afx_shutdown(void);

uint32_t afx_mem_alloc(uint32_t size, uint32_t align);
int afx_mem_free(uint32_t spu_addr);
int afx_mem_upload(uint32_t spu_addr, const void *data, uint32_t size);
int afx_mem_stats(afx_mem_stats_t *out_stats);
int afx_mem_diagnose(uint32_t size, uint32_t align, afx_mem_diagnostic_t *out_diagnostic);

/* Upload one contiguous sample-bank region. AFB loading owns the only public
 * sample allocation path; AFX relocations bind directly to its byte offsets. */
int afx_sample_bank_upload(const void *data, uint32_t bytes, afx_asset_t *out_bank);
/* Stream one backing allocation from caller-owned, aligned buffers. Begin one
 * DMA, fill another buffer while it runs, then poll and submit the next chunk. */
int afx_sample_bank_stream_begin(uint32_t bytes, afx_asset_t *out_bank);
int afx_sample_bank_stream_dma_begin(afx_asset_t bank, uint32_t byte_offset,
                                     const void *data, uint32_t bytes);
/* out_complete reports the submitted DMA, not the complete bank. */
int afx_sample_bank_stream_dma_poll(afx_asset_t bank, bool *out_complete);
int afx_sample_bank_stream_finish(afx_asset_t bank);
afx_asset_t afx_asset_upload(const void *data, uint32_t size, uint32_t align);
int afx_asset_free(afx_asset_t asset);
uint32_t afx_asset_addr(afx_asset_t asset);
uint32_t afx_asset_size(afx_asset_t asset);
int afx_asset_work(afx_asset_t asset, afx_work_profile_t *out_profile);

/* Stage 3 lifecycle API. Commands are nonblocking: a zero result means queued;
 * call afx_update and afx_instance_status for ARM-owned durable completion.
 * PATCH/gain/tempo/lane commands may follow ACTIVATE immediately; queue order
 * applies them before the first authored deadline. */
int afx_instance_activate(afx_asset_t flow_asset, afx_instance_t *out_instance);
/* Absolute AICA timer tick scheduled by activation; zero is an invalid/stale handle. */
uint32_t afx_instance_start_tick(afx_instance_t instance);
int afx_instance_stop(afx_instance_t instance);
int afx_instance_pause(afx_instance_t instance);
/* SH-4 has reconstructed complete unscaled channel words from a checkpoint.
 * Sample addresses are bank-relative; ARM7 resolves them and optionally re-keys voices. Position
 * is image-relative; staging remains host-owned until ARM acknowledges it. */
int afx_instance_rebuild(afx_instance_t instance, const afx_restore_channel_t *states,
                         uint32_t state_count, uint32_t stream_position,
                         uint32_t remaining_wait, uint32_t next_deadline, bool run);
/* Restore the latest AFC checkpoint at or before tick and resume. Requires a
 * paused instance. No AFX replay occurs between checkpoints. */
int afx_instance_seek(afx_instance_t instance, uint32_t tick);
/* As above; on success, optionally return the authored tick actually selected. */
int afx_instance_seek_checkpoint(afx_instance_t instance, uint32_t tick, uint32_t *out_tick);
/* Writes selected low register words to one mapped running/parked channel. The
 * values array contains popcount(mask) words in ascending field order.
 * CONTROL and SAMPLE_LOW are forbidden; use NOTE/KEYOFF for voice lifecycle.
 */
int afx_instance_patch(afx_instance_t instance, uint8_t local_channel,
                       uint32_t mask, const uint16_t *values);
/* Updates the effective gain for all current and future voices in one instance.
 * Gain is linear 0..255 and is composed with optional lane gain. */
int afx_instance_gain(afx_instance_t instance, uint8_t gain);
/* Sets the speed of one whole flow. 256 is authored tempo; accepted range is
 * 16..4096 (1/16x..16x). ARM7 keeps fractional ticks across authored waits. */
int afx_instance_tempo(afx_instance_t instance, uint16_t scale_q8_8);
/* Applies one persistent modifier to selected lanes. first_lane plus a set bit
 * in mask identifies a value in values[0..31]; callers coalesce changed lanes.
 * Gain and DSP-send values are linear 0..255; pan is a signed AICA pan offset;
 * a nonzero mute value releases current voices and suppresses later note-ons. */
int afx_instance_lanes_set(afx_instance_t instance, uint32_t modifier,
                           uint8_t first_lane, uint32_t mask, const uint8_t values[32]);
/* The one AICA DSP program belongs to the loaded scene, never to a flow.
 * Build a runtime image with <aicaflow/dsp.h>; this operation prepares,
 * validates and uploads it under the host lock. Replacement stops the old
 * scene before allocating the new ring; allocation failure leaves DSP off.
 * Scene teardown disables it and clears delay RAM before freeing the ring.
 *
 * SH4 owns the shared AICA asset arena. Programs with no MRD/MWT instructions
 * allocate no delay RAM. Programs with delay RAM allocate 128 KiB by default;
 * install the scene before loading assets to avoid fragmentation. A timeout
 * keeps the ring allocated: retry disable or shut down before reusing it. */
int afx_dsp_scene_program(const void *program, uint32_t bytes);
/* As afx_dsp_scene_program(), but selects the AICA delay-ring RBL value
 * (0..3: 8/16/32/64 Kiwords). A no-memory program automatically reserves
 * zero bytes regardless of rbl. Rings require a contiguous 2 KiB-aligned block. */
int afx_dsp_scene_program_ring(const void *program, uint32_t bytes, uint8_t rbl);
/* Gates the current scene program’s stereo returns without replacing its state. */
int afx_dsp_scene_returns(bool enabled);
int afx_dsp_scene_disable(void);
int afx_instance_recycle(afx_instance_t instance);
int afx_instance_status(afx_instance_t instance, afx_instance_status_t *out_status);
/* Refreshes ARM-owned instance observations; returns 0 even when none changed. */
int afx_update(void);

volatile afx_status_t *afx_status(void);
uint32_t afx_status_heartbeat(void);
uint32_t afx_status_timer_ticks(void);
/* Largest executor lateness, in AICA timer ticks, since this firmware boot. */
uint32_t afx_status_max_lateness(void);

#ifdef __cplusplus
}
#endif

#endif /* AICAFLOW_HOST_H */
