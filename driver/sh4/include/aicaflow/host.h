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
    uint32_t reference;
    uint32_t state;
    uint32_t sequence;
    uint32_t result;
    uint32_t position;
    uint32_t next_deadline;
    uint32_t detail;
} afx_instance_status_t;

typedef struct {
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t largest_free_block;
    uint32_t free_block_count;
    uint32_t active_allocations;
    uint32_t dynamic_base;
    uint32_t asset_limit;
} afx_mem_stats_t;
enum {
    AFX_MEM_AVAILABLE,
    AFX_MEM_FRAGMENTED,
    AFX_MEM_EXHAUSTED
};
typedef struct {
    uint32_t requested_bytes;
    uint32_t aligned_bytes;
    uint32_t free_bytes;
    uint32_t largest_free_block;
    uint32_t result;
} afx_mem_diagnostic_t;
typedef struct { uint32_t peak_commands, peak_register_writes; } afx_work_profile_t;

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

/* A mono PCM16/PCM8/AICA-ADPCM allocation. The complete sample uses one
 * aligned SH-4 -> AICA DMA transfer before this call returns. frames includes
 * any interpolation guard samples; rate/pitch is authored in the flow's setup
 * or NOTE. */
int afx_sample_upload(const void *data, uint32_t bytes, uint32_t frames,
                       uint32_t format, afx_asset_t *out_sample);
/* Upload one contiguous backing region, then expose its aligned subranges as
 * typed samples. This avoids one DMA and AICA allocation per bank member. */
int afx_sample_bank_upload(const void *data, uint32_t bytes, afx_asset_t *out_bank);
int afx_sample_view_create(afx_asset_t bank, uint32_t byte_offset, uint32_t bytes,
                           uint32_t frames, uint32_t format, afx_asset_t *out_sample);
/* Stream one backing allocation from caller-owned, aligned buffers. Begin one
 * DMA, fill another buffer while it runs, then poll and submit the next chunk. */
int afx_sample_bank_stream_begin(uint32_t bytes, afx_asset_t *out_bank);
int afx_sample_bank_stream_dma_begin(afx_asset_t bank, uint32_t byte_offset,
                                     const void *data, uint32_t bytes);
/* out_complete reports the submitted DMA, not the complete bank. */
int afx_sample_bank_stream_dma_poll(afx_asset_t bank, bool *out_complete);
int afx_sample_bank_stream_finish(afx_asset_t bank);
typedef struct {
    afx_asset_t sample;
    uint32_t byte_offset; /* PCM subrange; ADPCM must start at its encoded origin. */
    uint16_t fields[AFX_FIELD_COUNT]; /* Address/format bits and SA_LOW must be zero.
                                      * Loop, envelope, pitch, filter, send, etc. are authored. */
} afx_sfx_setup_t;
typedef struct {
    const afx_sfx_setup_t *setups;
    uint32_t setup_count;
    const void *stream; /* Existing little-endian AFX instruction encoding. */
    uint32_t stream_size;
    uint32_t required_channels, tick_rate_num, tick_rate_den;
    uint32_t flags; /* 0 for one-shots, AFX_FLAG_CONTROLLED to allow PARK. */
    /* Optional local-channel to lane map. Each byte is a lane id in 0..63. */
    const uint8_t *lane_map;
} afx_sfx_flow_t;
/* A flow whose samples are separately uploaded assets. It has no embedded
 * sample image or seek checkpoints. Music may use AFX_FLAG_MUSIC and lanes;
 * SFX callers should retain the narrower afx_sfx_flow_* API below. */
typedef afx_sfx_setup_t afx_external_setup_t;
typedef afx_sfx_flow_t afx_external_flow_t;
int afx_external_flow_begin(const afx_external_flow_t *recipe, afx_asset_t *out_flow);
int afx_external_flow_upload(const afx_external_flow_t *recipe, afx_asset_t *out_flow);
/* Copies the recipe, resolves sample handles and retains each distinct sample
 * until the flow is freed (including while unpublished). Noise setups use an
 * invalid sample handle and set the noise bit in CONTROL. Stream address
 * changes are rejected; select another bound setup with NOTE instead.
 * Complete/cancel begin with existing upload_step/asset_free. */
int afx_sfx_flow_begin(const afx_sfx_flow_t *recipe, afx_asset_t *out_flow);
int afx_sfx_flow_upload(const afx_sfx_flow_t *recipe, afx_asset_t *out_flow);

afx_asset_t afx_asset_upload(const void *data, uint32_t size, uint32_t align);
int afx_asset_free(afx_asset_t asset);
uint32_t afx_asset_addr(afx_asset_t asset);
uint32_t afx_asset_size(afx_asset_t asset);
int afx_asset_work(afx_asset_t asset, afx_work_profile_t *out_profile);

int afx_flow_upload(const void *flow_data, uint32_t flow_size, afx_asset_t *out_asset);
/* Links and compacts mutable caller storage. The buffer remains live until
 * afx_flow_release_host_image(); it avoids a second large SH-4 heap copy. */
int afx_flow_upload_inplace(void *flow_data, uint32_t flow_size, afx_asset_t *out_asset);
/* Streaming alternative for prevalidated AFX containers. Begin reserves the
 * resident image; upload accepts ordered image chunks; finish publishes it. */
int afx_flow_stream_begin(const afx_file_header_t *header, const void *checkpoints,
                          afx_asset_t *out_asset);
int afx_flow_stream_upload(afx_asset_t asset, uint32_t image_offset,
                           const void *data, uint32_t size);
/* DMA form for an aligned, persistent chunk. Start one transfer, then read the
 * next chunk while it runs; poll before reusing the source buffer. */
int afx_flow_stream_upload_dma_begin(afx_asset_t asset, uint32_t image_offset,
                                     const void *data, uint32_t size);
int afx_flow_stream_upload_dma_poll(afx_asset_t asset, bool *out_complete);
int afx_flow_stream_finish(afx_asset_t asset);
/* Begin validates, links and reserves the complete image, but it is not an
 * activatable asset until afx_flow_upload_step reports complete. Freeing this
 * handle cancels the upload and releases its reservation. */
int afx_flow_upload_begin(const void *flow_data, uint32_t flow_size, afx_asset_t *out_asset);
int afx_flow_upload_step(afx_asset_t asset, uint32_t max_bytes,
                         uint32_t *out_uploaded, bool *out_complete);
int afx_flow_upload_dma_step(afx_asset_t asset, uint32_t max_bytes,
                             uint32_t *out_uploaded, bool *out_complete);
/* Drops the linked SH-4 image after upload. Checkpoint seeks then read the
 * resident AICA image; calls before upload completion return -AFX_BUSY. */
int afx_flow_release_host_image(afx_asset_t asset);
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
 * The ARM7 only validates/copies them and optionally re-keys voices. Position
 * is image-relative; staging remains host-owned until ARM acknowledges it. */
int afx_instance_rebuild(afx_instance_t instance, const afx_restore_channel_t *states,
                         uint32_t state_count, uint32_t stream_position,
                         uint32_t local_tick, uint32_t next_deadline, bool run);
/* Replays from the latest compiler checkpoint not after tick on SH-4. The
 * instance must be paused; after afx_flow_release_host_image(), replay reads
 * the resident AICA image. Host-built SFX have no checkpoints and do not
 * support seek/raw rebuild. */
int afx_instance_seek(afx_instance_t instance, uint32_t tick);
/* Writes selected low register words to one mapped running/parked channel. The
 * values array contains popcount(mask) words in ascending field order.
 * Host-built SFX reject CONTROL/SA/LSA/LEA changes; their sample bindings are immutable. */
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
 * validates and uploads it atomically. Scene teardown disables it and clears
 * delay RAM. */
int afx_dsp_scene_program(const void *program, uint32_t bytes);
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
