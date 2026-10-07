/* Shared host/firmware wire ABI. */
#ifndef AICAFLOW_PROTOCOL_H
#define AICAFLOW_PROTOCOL_H
#include <aicaflow/format.h>
#include <aicaflow/limits.h>
#include <aicaflow/result.h>

/* ABI 7: bank-relative playback, compact state and allocated DSP rings.
 * Firmware/host must match.
 * Shared with the assembler and linker. All wire integers are LE. */
#define AFX_ABI_VERSION 7
#define AFX_FIRMWARE_MAGIC 0x32524641
#define AFX_STATUS_MAGIC 0x32534641
#define AFX_LAYOUT_ID 0x20261009
#define AFX_FIRMWARE_INFO_OFFSET 0x20
#define AFX_FIRMWARE_INFO_BYTES 32
#define AFX_AICA_RAM_SIZE 0x200000
#define AFX_AICA_REG_BASE 0x800000
#define AFX_AICA_CHANNEL_COUNT 64
#define AFX_AICA_CHANNEL_REG_STRIDE 0x80
#define AFX_MAX_FLOW_SLOTS 64
#define AFX_CONTROL_BASE 0x1fcec0
/* DSP rings are scene-owned arena allocations, aligned to RBP's 2 KiB units.
 * RBL 0..3 selects 16/32/64/128 KiB. Programs may also use no external RAM. */
#define AFX_DSP_BYTES 0x20000
#define AFX_DSP_MIN_BYTES 0x4000
#define AFX_DSP_RING_ALIGN 2048
#define AFX_DSP_RING_NONE 4u
#define AFX_ASSET_MAX AFX_CONTROL_BASE
#define AFX_UPLOAD_ALIGN 32
#define AFX_STATUS_ADDR AFX_CONTROL_BASE
#define AFX_STATUS_BYTES 80
/* Keep the producer, consumer and command array on 32-byte boundaries. */
#define AFX_QUEUE_ADDR (AFX_AICA_RAM_SIZE - AFX_QUEUE_HEADER_BYTES - AFX_CMD_QUEUE_CAPACITY * AFX_CMD_BYTES)
#define AFX_CMD_QUEUE_CAPACITY 32
#define AFX_CMD_BYTES 64
#define AFX_QUEUE_HEADER_BYTES 64
#define AFX_OBSERVED_ADDR (AFX_STATUS_ADDR + AFX_STATUS_BYTES)
#define AFX_OBSERVED_BYTES 32
#define AFX_CHANNEL_MAP_ARENA_ADDR (AFX_OBSERVED_ADDR + AFX_MAX_FLOW_SLOTS * AFX_OBSERVED_BYTES)
#define AFX_CHANNEL_MAP_ARENAS 5
#define AFX_CHANNEL_MAP_ENTRIES 64
#define AFX_CHANNEL_MAP_ENTRY_BYTES 1
#define AFX_CHANNEL_MAP_ALLOC_ALIGN 4
#define AFX_CHANNEL_MAP_ARENA_SIZE (AFX_CHANNEL_MAP_ENTRIES * AFX_CHANNEL_MAP_ENTRY_BYTES)
#define AFX_PRIVATE_BASE (AFX_CHANNEL_MAP_ARENA_ADDR + AFX_CHANNEL_MAP_ARENAS * AFX_CHANNEL_MAP_ARENA_SIZE)
#define AFX_CLOCK_BASE (AFX_QUEUE_ADDR - 32)
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
    uint32_t magic; /* AFX_FIRMWARE_MAGIC identifies a driver image. */
    uint32_t abi; /* Required host/firmware ABI version. */
    uint32_t layout_id; /* Fixed AICA RAM layout identifier. */
    uint32_t load_bytes; /* Firmware bytes to upload at AICA address zero. */
    uint32_t asset_base; /* First usable AICA asset byte address, upload-aligned. */
    uint32_t asset_limit; /* Exclusive fixed asset-arena ceiling, including DSP allocations. */
    uint32_t private_end; /* Exclusive end of ARM7 private data/BSS in AICA RAM. */
    uint32_t stack_base; /* Lowest reserved stack byte address in AICA RAM. */
} afx_firmware_info_t;

typedef struct {
    uint32_t opcode; /* AFX_CMD_* operation. */
    uint32_t sequence; /* Host-assigned command sequence for completion matching. */
    uint32_t reference; /* Instance handle, or AFX_DSP_SCENE_REFERENCE. */
    uint32_t flags; /* Opcode-specific options; zero when unused. */
    uint8_t payload[48]; /* Opcode-specific payload; unused bytes are zero. */
} afx_cmd_t;
typedef struct {
    volatile uint32_t head; /* SH-4 producer counter; wraps modulo 2^32. */
    uint32_t reserved_head[7]; /* Padding keeps head and tail in separate 32-byte regions. */
    volatile uint32_t tail; /* ARM7 consumer counter; wraps modulo 2^32. */
    uint32_t reserved_tail[7]; /* Padding aligns the command array to 32 bytes. */
    afx_cmd_t commands[AFX_CMD_QUEUE_CAPACITY]; /* Fixed-size ring entries, indexed by counter modulo capacity. */
} afx_cmd_queue_t;
typedef struct {
    volatile uint32_t epoch; /* Publication counter: odd while ARM7 writes, even when stable. */
    volatile uint32_t reference; /* Instance handle; zero denotes a recycled free slot. */
    volatile uint32_t state; /* AFX_FREE/RUNNING/PAUSED/PARKED/DONE/ERROR. */
    volatile uint32_t sequence; /* Sequence associated with this observation. */
    volatile uint32_t result; /* Nonnegative afx_result_t from ARM7. */
    volatile uint32_t position; /* Byte offset from the uploaded AFX image base. */
    volatile uint32_t next_deadline; /* Next scheduled deadline in wrapping hardware timer ticks. */
    volatile uint32_t detail; /* State-specific diagnostic: lateness, channel or error detail. */
} afx_observed_t; /* ARM-only writes; odd epoch while publishing. */
typedef struct {
    uint32_t image_base; /* Absolute AICA byte address of the uploaded AFX image. */
    uint32_t image_size; /* Uploaded image length in bytes. */
    uint32_t stream_offset; /* First instruction byte offset relative to image_base. */
    uint32_t stream_size; /* Instruction stream length in bytes. */
    uint32_t setups_offset; /* Setup-table byte offset relative to image_base. */
    uint32_t setup_count; /* Number of AFX_SETUP_BYTES templates, at most 65536. */
    uint32_t channel_map; /* Absolute AICA byte address of the local-to-physical channel map. */
    uint32_t required_channels; /* Number of map entries/voices to bind, 1..64. */
    uint32_t flags; /* Accepted AFX_FLAG_* playback options. */
    uint32_t start_tick; /* First execution deadline in wrapping hardware timer ticks. */
    uint16_t bank_base_units; /* Bound bank AICA byte address divided by 32; zero without a bank. */
    uint16_t reserved; /* Must be zero. */
    uint32_t bank_bytes; /* Bound bank payload length in bytes; zero for a bankless flow. */
} afx_activation_t;
typedef struct {
    uint8_t local_channel; /* Local channel index within the instance. */
    uint8_t reserved[3]; /* Must be zero. */
    uint32_t mask; /* AFX_PATCH_FIELD_MASK subset selecting register words. */
    uint16_t values[AFX_FIELD_COUNT]; /* Selected 16-bit register values in ascending field order. */
    uint32_t reserved_tail; /* Must be zero. */
} afx_patch_payload_t;
typedef struct {
    uint32_t states_address; /* Absolute AICA byte address of temporary restore records. */
    uint32_t state_count; /* Number of afx_restore_channel_t records. */
    uint32_t stream_position; /* Resume byte offset relative to the uploaded AFX image. */
    uint32_t remaining_wait; /* Authored ticks left before the next instruction, not song position. */
    uint32_t next_deadline; /* Hardware timer deadline at which restored execution resumes. */
    uint32_t reserved[7]; /* Must be zero. */
} afx_rebuild_payload_t;
typedef afx_checkpoint_channel_t afx_restore_channel_t;
typedef struct {
    uint32_t ring_address; /* Allocated, 2 KiB-aligned AICA ring base; zero without a ring. */
    uint32_t ring_bytes; /* 0, 16, 32, 64 or 128 KiB. */
    uint32_t reserved[10]; /* Must be zero; DISABLE requires an entirely zero payload. */
} afx_dsp_payload_t;
typedef struct {
    uint32_t gain; /* Linear instance gain, 0=silent and 255=unity. */
    uint32_t reserved[11]; /* Must be zero. */
} afx_gain_payload_t;
typedef struct {
    uint32_t period_q8_8; /* Hardware ticks per authored tick in Q8.8; 256=normal, 16..4096. */
    uint32_t reserved[11]; /* Must be zero. */
} afx_tempo_payload_t;
/* One command changes up to 32 consecutive lane values. Mask bits select values
 * in the compact values array by lane-relative index. */
typedef struct {
    uint32_t first_lane; /* First authored lane addressed by mask and values. */
    uint32_t mask; /* Bit n selects lane first_lane+n. */
    uint8_t values[32]; /* One value per relative lane index, interpreted by the modifier. */
    uint32_t reserved[2]; /* Must be zero. */
} afx_lane_payload_t;
#define AFX_REBUILD_RUN 1u
typedef struct {
    volatile uint32_t magic; /* AFX_STATUS_MAGIC, published after bootstrap completes. */
    volatile uint32_t abi; /* Running firmware ABI version. */
    volatile uint32_t layout_id; /* Running firmware fixed-layout identifier. */
    volatile uint32_t capabilities; /* Supported AFX_CAP_* features. */
    volatile uint32_t heartbeat; /* Main-loop iteration counter; wraps modulo 2^32. */
    volatile uint32_t timer_ticks; /* Latest main-loop snapshot of the hardware timer clock. */
    volatile uint32_t asset_base; /* First usable AICA asset byte address. */
    volatile uint32_t asset_limit; /* Exclusive fixed asset-arena ceiling, including DSP allocations. */
    volatile uint32_t private_end; /* Exclusive end of ARM7 private data/BSS. */
    volatile uint32_t stack_base; /* Lowest reserved stack byte address. */
    volatile uint32_t error; /* Global nonnegative afx_result_t; AFX_OK when healthy. */
    volatile uint32_t reserved; /* Bootstrap/command diagnostic breadcrumb, despite the legacy name. */
    volatile uint32_t dsp_sequence; /* Last completed DSP scene command sequence. */
    volatile uint32_t dsp_result; /* Nonnegative result of that DSP scene command. */
    volatile uint32_t max_lateness; /* Largest observed playback lateness in hardware timer ticks. */
    volatile uint32_t stack_free[5]; /* Untouched stack bytes measured at boot: SVC, FIQ, IRQ, ABT, UND. */
} afx_status_t; /* ARM owns all fields, magic published last. */
typedef struct {
    uint32_t pc; /* Absolute AICA byte address of the next instruction. */
    uint32_t end; /* Exclusive absolute instruction-stream end address. */
    uint32_t setups; /* Absolute AICA byte address of the setup table. */
    uint32_t setup_count; /* Number of setup templates, at most 65536. */
    uint32_t channel_map; /* Absolute AICA byte address of the byte-sized channel map. */
    uint32_t deadline; /* Next event deadline in wrapping hardware timer ticks. */
    uint32_t remaining_wait; /* Authored wait ticks not yet scheduled, including long-WAIT chunks. */
    uint16_t flags; /* Validated AFX playback flags. */
    uint8_t gain; /* Instance gain, 0=silent and 255=unity. */
    uint8_t lane_count; /* Number of addressable lanes, at most 64. */
    uint32_t reference; /* Instance handle; zero means this runtime slot is unused. */
    uint32_t sequence; /* Lifecycle sequence used when publishing playback state. */
    uint32_t image_base; /* Absolute AICA image base for reporting relative positions. */
    uint32_t stream_start; /* First instruction byte offset relative to image_base. */
    uint32_t max_lateness; /* Largest lateness for this instance in hardware timer ticks. */
    uint32_t bank_end; /* Exclusive absolute bank byte address for sample bounds checks. */
    uint16_t bank_base_units; /* Absolute bank byte address divided by 32. */
    uint16_t tempo_period_q8_8; /* Hardware ticks per authored tick in Q8.8; 256=normal. */
    uint8_t channels; /* Number of bound local channels, 1..64 while active. */
    uint8_t state; /* Current AFX_* playback state. */
    uint8_t tempo_fraction; /* Fractional hardware tick carried between scaled waits, 0..255. */
    uint8_t reserved; /* Unused byte, cleared with the context. */
} afx_runtime_slot_t;

#if defined(__cplusplus)
#define AFX_ASSERT static_assert
#else
#define AFX_ASSERT _Static_assert
#endif
AFX_ASSERT(sizeof(afx_firmware_info_t) == AFX_FIRMWARE_INFO_BYTES, "firmware manifest");
AFX_ASSERT(sizeof(afx_cmd_t) == AFX_CMD_BYTES, "IPC command");
AFX_ASSERT(sizeof(afx_observed_t) == AFX_OBSERVED_BYTES, "observed record");
AFX_ASSERT(sizeof(afx_activation_t) == 48 && sizeof(afx_runtime_slot_t) == 64, "context");
AFX_ASSERT(sizeof(afx_patch_payload_t) == 48 && offsetof(afx_patch_payload_t, values) == 8, "IPC fields");
AFX_ASSERT(sizeof(afx_rebuild_payload_t) == 48 && sizeof(afx_gain_payload_t) == 48 &&
           sizeof(afx_tempo_payload_t) == 48 && sizeof(afx_dsp_payload_t) == 48 &&
           sizeof(afx_lane_payload_t) == 48, "IPC transport");
AFX_ASSERT(sizeof(afx_restore_channel_t) == 40, "prepared channel state");
AFX_ASSERT(AFX_ASSET_MAX == AFX_TARGET_MAX_BANK_BYTES, "public target bank limit");
AFX_ASSERT(offsetof(afx_cmd_queue_t, commands) == AFX_QUEUE_HEADER_BYTES, "IPC payload alignment");
AFX_ASSERT((AFX_CMD_QUEUE_CAPACITY & (AFX_CMD_QUEUE_CAPACITY - 1u)) == 0 &&
           AFX_CMD_QUEUE_CAPACITY > 0, "IPC ring capacity must be a power of two");
AFX_ASSERT(sizeof(afx_cmd_queue_t) == 2112, "32-entry IPC queue");
AFX_ASSERT((AFX_CONTROL_BASE & 31u) == 0, "asset ceiling alignment");
AFX_ASSERT(sizeof(afx_status_t) == AFX_STATUS_BYTES, "status layout size");
AFX_ASSERT(AFX_STATUS_ADDR + sizeof(afx_status_t) == AFX_OBSERVED_ADDR, "status/observed adjacency");
AFX_ASSERT((AFX_QUEUE_ADDR & 31u) == 0, "queue alignment");
AFX_ASSERT(AFX_QUEUE_ADDR + sizeof(afx_cmd_queue_t) == AFX_AICA_RAM_SIZE, "queue at RAM top");
AFX_ASSERT(AFX_OBSERVED_ADDR + 64 * sizeof(afx_observed_t) == AFX_CHANNEL_MAP_ARENA_ADDR, "observed overlap");
AFX_ASSERT(AFX_CHANNEL_MAP_ARENA_ADDR + AFX_CHANNEL_MAP_ARENAS * AFX_CHANNEL_MAP_ARENA_SIZE == AFX_PRIVATE_BASE, "maps overlap");
AFX_ASSERT(AFX_PRIVATE_BASE + 64 * sizeof(afx_runtime_slot_t) <= AFX_STACK_BASE, "private overlap");
#undef AFX_ASSERT
#endif
#endif
