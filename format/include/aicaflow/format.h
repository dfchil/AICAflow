/* Portable asset-format contract. No firmware, IPC or runtime allocation state. */
#ifndef AICAFLOW_FORMAT_H
#define AICAFLOW_FORMAT_H

/* Version of this public source interface, independent of repository releases,
 * asset-format versions below, and the host/firmware ABI in protocol.h. */
#define AFX_FORMAT_API_VERSION 1
#define AFX_FILE_MAGIC 0x32584641
#define AFX_CHECKPOINT_MAGIC 0x31504b43
#define AFX_CHECKPOINT_VERSION 1
#define AFX_MAX_FLOW_CHANNELS 64

#ifndef __ASSEMBLER__
#include <stdint.h>

/* One field is the low 16-bit word at channel register offset field*4.
 * Reserved bits remain caller responsibility; masks cannot address unused words. */
enum {
    AFX_FIELD_CONTROL, AFX_FIELD_SAMPLE_LOW, AFX_FIELD_LOOP_START,
    AFX_FIELD_LOOP_END, AFX_FIELD_ENV_AD, AFX_FIELD_ENV_DR, AFX_FIELD_PITCH,
    /* 0x20 is DSP sends, 0x24 is direct pan/filter-Q, and 0x28 is the
     * mixer TL/LPF word. Each field is one whole AICA register word. */
    AFX_FIELD_LFO, AFX_FIELD_DSP_SEND, AFX_FIELD_DIRECT, AFX_FIELD_MIX,
    AFX_FIELD_FILTER_LEVEL0, AFX_FIELD_FILTER_LEVEL1, AFX_FIELD_FILTER_LEVEL2,
    AFX_FIELD_FILTER_LEVEL3, AFX_FIELD_FILTER_LEVEL4, AFX_FIELD_FILTER_AD,
    AFX_FIELD_FILTER_DR, AFX_FIELD_COUNT
};
#define AFX_FIELD_TOTAL_LEVEL AFX_FIELD_MIX
#define AFX_FIELD_MASK ((1u << AFX_FIELD_COUNT) - 1u)
#define AFX_SETUP_BYTES (AFX_FIELD_COUNT * 2u)
#define AFX_NOTE_PL_MASK ((1u << AFX_FIELD_PITCH) | (1u << AFX_FIELD_TOTAL_LEVEL))

enum {
    AFX_OP_END = 0, AFX_OP_WAIT8 = 1, AFX_OP_WAIT16 = 2, AFX_OP_WAIT32 = 3,
    AFX_OP_NOTE = 0x10, AFX_OP_PATCH = 0x11, AFX_OP_KEYOFF = 0x12,
    AFX_OP_PARK = 0x13, AFX_OP_NOTE_PL = 0x14, AFX_OP_PATCH_LEVEL = 0x15
};
enum { AFX_FLAG_CONTROLLED = 1, AFX_FLAG_MUSIC = 2, AFX_FLAG_METADATA = 4,
       AFX_FLAG_MUSIC_CHORUS = 8, AFX_FLAG_LANES = 16 };

#define AFX_METADATA_MAGIC 0x314d5841u
#define AFX_CONTAINER_VERSION 1u
#define AFX_FILE_VERSION 7u /* Bank-bound, sample-free AFX container. */
/* AFB and AFC both have a fixed 32-byte little-endian header. They are
 * format constants, not SH4 implementation details: offline C authoring
 * tools and the host loader must agree on them. */
#define AFX_BANK_MAGIC 0x00424641u /* "AFB\\0" in little-endian byte order. */
#define AFX_BANK_VERSION 1u
#define AFX_BANK_HEADER_BYTES 32u
#define AFX_SEEK_MAGIC 0x00434641u /* "AFC\\0" in little-endian byte order. */
#define AFX_SEEK_VERSION 1u
#define AFX_SEEK_HEADER_BYTES 32u
/* AFI is an AFB sample catalog for SH4-side one-shots. It is never uploaded
 * to AICA or interpreted by the ARM7. */
#define AFX_INDEX_MAGIC 0x00494641u /* "AFI\\0" in little-endian byte order. */
#define AFX_INDEX_VERSION 1u
#define AFX_INDEX_HEADER_BYTES 32u
enum { AFX_INDEX_RECORD_BYTES = 16u, AFX_INDEX_NAMED_RECORD_BYTES = 32u };
enum { AFX_PCM16 = 0, AFX_PCM8 = 1, AFX_ADPCM = 2 };

#define AFX_FILE_HEADER_BYTES 80u
#define AFX_WORK_PROFILE(commands, writes) (((uint32_t)(commands) << 16) | (uint16_t)(writes))
#define AFX_WORK_PROFILE_COMMANDS(profile) ((profile) >> 16)
#define AFX_WORK_PROFILE_WRITES(profile) ((profile) & 0xffffu)
typedef struct {
    uint32_t magic, abi, total_size, flags;
    uint32_t image_offset, image_size, stream_offset, stream_size;
    /* AFX version 7: control_id identifies an optional SH-4 seek sidecar; bank_id
     * identifies the sole AFB payload.  No metadata follows this header. */
    uint32_t control_id, setup_count, bank_id_low, bank_id_high;
    uint32_t relocations_offset, relocation_count, reserved0, reserved1;
    uint32_t required_channels, tick_rate_num, tick_rate_den, work_profile;
} afx_file_header_t;
typedef struct { uint32_t image_offset, byte_size, frames, format; } afx_sample_t;
typedef struct { uint32_t pair_offset, sample_index, byte_offset; } afx_relocation_t;

/* One AFC checkpoint channel record; also used by runtime rebuild requests. */
typedef struct {
    uint32_t local_channel;
    uint16_t fields[AFX_FIELD_COUNT];
} afx_checkpoint_channel_t;

#if defined(__cplusplus)
#define AFX_FORMAT_ASSERT static_assert
#else
#define AFX_FORMAT_ASSERT _Static_assert
#endif
AFX_FORMAT_ASSERT(sizeof(afx_file_header_t) == 80, "file header");
AFX_FORMAT_ASSERT(sizeof(afx_sample_t) == 16 && sizeof(afx_relocation_t) == 12, "host tables");
AFX_FORMAT_ASSERT(sizeof(afx_checkpoint_channel_t) == 40, "checkpoint channel");
#undef AFX_FORMAT_ASSERT
#endif
#endif
