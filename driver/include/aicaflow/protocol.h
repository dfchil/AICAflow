/* Shared host/firmware wire ABI. */
#ifndef AICAFLOW_PROTOCOL_H
#define AICAFLOW_PROTOCOL_H
#include <aicaflow/format.h>
#include <aicaflow/limits.h>
#include <aicaflow/result.h>

/* ABI 6: optional lane maps and per-instance modifiers. Firmware/host must match.
 * Shared with the assembler and linker. All wire integers are LE. */
#define AFX_ABI_VERSION 6
#define AFX_FIRMWARE_MAGIC 0x32524641
#define AFX_STATUS_MAGIC 0x32534641
#define AFX_LAYOUT_ID 0x20260925
#define AFX_FIRMWARE_INFO_OFFSET 0x20
#define AFX_FIRMWARE_INFO_BYTES 32
#define AFX_AICA_RAM_SIZE 0x200000
#define AFX_AICA_REG_BASE 0x800000
#define AFX_AICA_CHANNEL_COUNT 64
#define AFX_AICA_CHANNEL_REG_STRIDE 0x80
#define AFX_MAX_FLOW_SLOTS 64
#define AFX_CONTROL_BASE 0x1fc000
/* DSP delay RAM is a scene-owned top reservation.  RBL 0..3 selects
 * 8/16/32/64 Kiwords (16/32/64/128 KiB); the DSP itself also runs without
 * external delay RAM.  Assets are always allocated below the active ring. */
#define AFX_DSP_BYTES 0x20000
#define AFX_DSP_MIN_BYTES 0x4000
#define AFX_DSP_MAX_BASE (AFX_CONTROL_BASE - AFX_DSP_BYTES)
#define AFX_DSP_RING_NONE 4u
#define AFX_ASSET_MAX AFX_CONTROL_BASE
#define AFX_UPLOAD_ALIGN 32
#define AFX_STATUS_ADDR AFX_CONTROL_BASE
#define AFX_QUEUE_ADDR (AFX_CONTROL_BASE + 0x100)
#define AFX_CMD_QUEUE_CAPACITY 64
#define AFX_CMD_BYTES 64
#define AFX_OBSERVED_ADDR (AFX_CONTROL_BASE + 0x1200)
#define AFX_OBSERVED_BYTES 32
#define AFX_CHANNEL_MAP_ARENA_ADDR (AFX_CONTROL_BASE + 0x1a00)
#define AFX_CHANNEL_MAP_ARENAS 5
#define AFX_CHANNEL_MAP_ENTRIES 64
#define AFX_CHANNEL_MAP_ENTRY_BYTES 4
#define AFX_CHANNEL_MAP_ARENA_SIZE (AFX_CHANNEL_MAP_ENTRIES * AFX_CHANNEL_MAP_ENTRY_BYTES)
#define AFX_PRIVATE_BASE (AFX_CHANNEL_MAP_ARENA_ADDR + AFX_CHANNEL_MAP_ARENAS * AFX_CHANNEL_MAP_ARENA_SIZE)
#define AFX_CLOCK_BASE 0x1fffe0
#define AFX_AICA_TIMER_TICK_ADDR AFX_CLOCK_BASE
#define AFX_EXCEPTION_STACK_BYTES 256
#define AFX_MAIN_STACK_BYTES 2048
#define AFX_UND_STACK_TOP AFX_CLOCK_BASE
#define AFX_ABT_STACK_TOP (AFX_UND_STACK_TOP - AFX_EXCEPTION_STACK_BYTES)
#define AFX_IRQ_STACK_TOP (AFX_ABT_STACK_TOP - AFX_EXCEPTION_STACK_BYTES)
#define AFX_FIQ_STACK_TOP (AFX_IRQ_STACK_TOP - AFX_EXCEPTION_STACK_BYTES)
#define AFX_SVC_STACK_TOP (AFX_FIQ_STACK_TOP - AFX_EXCEPTION_STACK_BYTES)
#define AFX_STACK_BASE (AFX_SVC_STACK_TOP - AFX_MAIN_STACK_BYTES)
#define AFX_STACK_PATTERN 0xa5a5a5a5
#define AFX_TIMER_RELOAD 212

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

typedef uint32_t afx_handle_t;
#define AFX_HANDLE_INVALID 0u
#define AFX_DSP_SCENE_REFERENCE UINT32_MAX
/* Index+1 in low half, nonzero generation in high half; retire on wrap.
 * Handle type is host-side, not encoded. Flow references use the same encoding. */
#define AFX_MAKE_HANDLE(index, generation) (((uint32_t)(generation) << 16) | ((index) + 1u))
#define AFX_HANDLE_INDEX(handle) (((handle) & 0xffffu) - 1u)
#define AFX_HANDLE_GENERATION(handle) ((handle) >> 16)

enum { AFX_FREE, AFX_RUNNING, AFX_PAUSED, AFX_PARKED, AFX_DONE, AFX_ERROR };
enum {
    AFX_CMD_NOP, AFX_CMD_ACTIVATE, AFX_CMD_STOP, AFX_CMD_PAUSE,
    AFX_CMD_REBUILD, AFX_CMD_RECYCLE, AFX_CMD_PATCH, AFX_CMD_INSTANCE_GAIN,
    AFX_CMD_INSTANCE_TEMPO, AFX_CMD_LANE_SET, AFX_CMD_DSP_ENABLE, AFX_CMD_DSP_DISABLE
};
#define AFX_KEYON 0x4000u
#define AFX_KEYON_EXECUTE 0x8000u
#define AFX_EXECUTOR_MAX_EVENTS_PER_PASS 32u
#define AFX_EXECUTOR_MAX_COMMANDS_PER_PASS 8u
enum { AFX_LANE_GAIN, AFX_LANE_MUTE, AFX_LANE_PAN, AFX_LANE_DSP_SEND,
       AFX_LANE_MODIFIER_COUNT };
enum { AFX_CAP_BOOTSTRAP = 1, AFX_CAP_LIFECYCLE = 2, AFX_CAP_PLAYBACK = 4, AFX_CAP_DSP = 8 };

typedef struct {
    uint32_t magic, abi, layout_id, load_bytes, asset_base, asset_limit;
    uint32_t private_end, stack_base;
} afx_firmware_info_t;

typedef struct {
    uint32_t opcode, sequence, reference, flags;
    uint8_t payload[48];
} afx_cmd_t;
typedef struct {
    volatile uint32_t head; uint32_t reserved_head[7]; /* SH4 publishes */
    volatile uint32_t tail; uint32_t reserved_tail[7]; /* ARM publishes */
    afx_cmd_t commands[AFX_CMD_QUEUE_CAPACITY];
} afx_cmd_queue_t;
typedef struct {
    volatile uint32_t epoch, reference, state, sequence;
    volatile uint32_t result, position, next_deadline, detail;
} afx_observed_t; /* ARM-only writes; odd epoch while publishing. */
typedef struct {
    uint32_t image_base, image_size, stream_offset, stream_size;
    uint32_t setups_offset, setup_count, channel_map, required_channels;
    uint32_t flags, start_tick, reserved[2];
} afx_activation_t;
typedef struct {
    uint8_t local_channel, reserved[3];
    uint32_t mask;
    uint16_t values[AFX_FIELD_COUNT]; /* Packed selected words, ascending field id. */
    uint32_t reserved_tail;
} afx_patch_payload_t;
typedef struct {
    uint32_t states_address, state_count, stream_position, local_tick, next_deadline;
    uint32_t reserved[7];
} afx_rebuild_payload_t;
typedef afx_checkpoint_channel_t afx_restore_channel_t;
typedef struct { uint32_t gain, reserved[11]; } afx_gain_payload_t;
typedef struct { uint32_t period_q8_8, reserved[11]; } afx_tempo_payload_t;
/* One command changes up to 32 consecutive lane values. Mask bits select values
 * in the compact values array by lane-relative index. */
typedef struct {
    uint32_t first_lane, mask;
    uint8_t values[32];
    uint32_t reserved[2];
} afx_lane_payload_t;
#define AFX_REBUILD_RUN 1u
typedef struct {
    volatile uint32_t magic, abi, layout_id, capabilities, heartbeat, timer_ticks;
    volatile uint32_t asset_base, asset_limit, private_end, stack_base, error, reserved;
    volatile uint32_t dsp_sequence, dsp_result, max_lateness;
    volatile uint32_t stack_free[5]; /* SVC, FIQ, IRQ, ABT, UND */
} afx_status_t; /* ARM owns all fields, magic published last. */
typedef struct {
    uint32_t pc, end, setups, setup_count, channel_map, channels;
    uint32_t deadline, local_tick, flags, reference, state, sequence;
    uint32_t image_base, stream_start, max_lateness, tempo_period_q8_8, tempo_fraction;
} afx_runtime_slot_t;

#if defined(__cplusplus)
#define AFX_ASSERT static_assert
#else
#define AFX_ASSERT _Static_assert
#endif
AFX_ASSERT(sizeof(afx_firmware_info_t) == AFX_FIRMWARE_INFO_BYTES, "firmware manifest");
AFX_ASSERT(sizeof(afx_cmd_t) == AFX_CMD_BYTES, "IPC command");
AFX_ASSERT(sizeof(afx_observed_t) == AFX_OBSERVED_BYTES, "observed record");
AFX_ASSERT(sizeof(afx_activation_t) == 48 && sizeof(afx_runtime_slot_t) == 68, "context");
AFX_ASSERT(sizeof(afx_patch_payload_t) == 48 && offsetof(afx_patch_payload_t, values) == 8, "IPC fields");
AFX_ASSERT(sizeof(afx_rebuild_payload_t) == 48 && sizeof(afx_gain_payload_t) == 48 &&
           sizeof(afx_tempo_payload_t) == 48 &&
           sizeof(afx_lane_payload_t) == 48, "IPC transport");
AFX_ASSERT(sizeof(afx_restore_channel_t) == 40, "prepared channel state");
AFX_ASSERT(AFX_ASSET_MAX == AFX_TARGET_MAX_BANK_BYTES, "public target bank limit");
AFX_ASSERT(offsetof(afx_cmd_queue_t, commands) == 64, "IPC payload alignment");
AFX_ASSERT(sizeof(afx_status_t) <= AFX_QUEUE_ADDR - AFX_STATUS_ADDR, "status overlap");
AFX_ASSERT(AFX_QUEUE_ADDR + sizeof(afx_cmd_queue_t) <= AFX_OBSERVED_ADDR, "queue overlap");
AFX_ASSERT(AFX_OBSERVED_ADDR + 64 * sizeof(afx_observed_t) == AFX_CHANNEL_MAP_ARENA_ADDR, "observed overlap");
AFX_ASSERT(AFX_CHANNEL_MAP_ARENA_ADDR + AFX_CHANNEL_MAP_ARENAS * AFX_CHANNEL_MAP_ARENA_SIZE == AFX_PRIVATE_BASE, "maps overlap");
AFX_ASSERT(AFX_PRIVATE_BASE + 64 * sizeof(afx_runtime_slot_t) <= AFX_STACK_BASE, "private overlap");
AFX_ASSERT((AFX_DSP_MAX_BASE & 2047) == 0, "DSP alignment");
#undef AFX_ASSERT
#endif
#endif
