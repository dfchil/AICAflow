/* Portable asset-format contract. No firmware, IPC or runtime allocation state. */
#ifndef AICAFLOW_FORMAT_H
#define AICAFLOW_FORMAT_H

/* Version of this public source interface, independent of repository releases,
 * asset-format versions below, and the host/firmware ABI in protocol.h. */
#define AFX_FORMAT_API_VERSION 6
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
    /* 0x20 is DSP sends, 0x24 is direct pan/level, and 0x28 is total-level
     * attenuation, filter Q and bypass flags. Each field is one register word. */
    AFX_FIELD_LFO, AFX_FIELD_DSP_SEND, AFX_FIELD_DIRECT, AFX_FIELD_MIX,
    AFX_FIELD_FILTER_LEVEL0, AFX_FIELD_FILTER_LEVEL1, AFX_FIELD_FILTER_LEVEL2,
    AFX_FIELD_FILTER_LEVEL3, AFX_FIELD_FILTER_LEVEL4, AFX_FIELD_FILTER_AD,
    AFX_FIELD_FILTER_DR, AFX_FIELD_COUNT
};
#define AFX_FIELD_TOTAL_LEVEL AFX_FIELD_MIX
#define AFX_FIELD_MASK ((1u << AFX_FIELD_COUNT) - 1u)
/* PATCH cannot change sample binding, format, loop mode or key flags. */
#define AFX_PATCH_FIELD_MASK (AFX_FIELD_MASK & ~((1u << AFX_FIELD_CONTROL) | (1u << AFX_FIELD_SAMPLE_LOW)))
#define AFX_SETUP_BYTES (AFX_FIELD_COUNT * 2u)
#define AFX_NOTE_PL_MASK ((1u << AFX_FIELD_PITCH) | (1u << AFX_FIELD_TOTAL_LEVEL))
/* The fields of the compact PATCH forms: a voice's level, its envelope rates, or both. */
#define AFX_PATCH_LEVEL_MASK (1u << AFX_FIELD_TOTAL_LEVEL)
#define AFX_PATCH_ENV_MASK (1u << AFX_FIELD_ENV_AD)
#define AFX_PATCH_ENV_LEVEL_MASK (AFX_PATCH_ENV_MASK | AFX_PATCH_LEVEL_MASK)

enum {
    AFX_OP_END = 0, AFX_OP_WAIT8 = 1, AFX_OP_WAIT16 = 2, AFX_OP_WAIT32 = 3,
    AFX_OP_NOTE = 0x10, AFX_OP_PATCH = 0x11, AFX_OP_KEYOFF = 0x12,
    AFX_OP_PARK = 0x13, AFX_OP_NOTE_PL = 0x14, AFX_OP_PATCH_LEVEL = 0x15,
    /* 0x16 was LOOP, gone with format API 6: a song's continue join replaces it. */
    AFX_OP_PATCH_ENV = 0x17, AFX_OP_PATCH_ENV_LEVEL = 0x18,
    /* Songs only (AFM): NEXT mode ends a section's body; FREE channel gives a
     * local channel's voice back to the song's pool. */
    AFX_OP_NEXT = 0x19, AFX_OP_FREE = 0x1a
};
/* How a song goes on at a section's NEXT: the next section starts on the
 * song's other executor with voices of its own while this one plays its
 * tail, or this executor jumps to it and keeps its voices. */
enum { AFX_JOIN_NEW = 0, AFX_JOIN_CONTINUE = 1 };
/* AFX_FLAG_BANKS: the flow plays samples from up to AFX_MAX_FLOW_BANKS banks
 * (see AFX_BANK_SLOT_SHIFT). AFX_FLAG_SONG and AFX_FLAG_POOL mark a song's
 * activation, not a file: a song, and one with a new-play join, which plays
 * from a voice pool with a runtime block of its own. Bit 32 was LOOP. */
enum { AFX_FLAG_CONTROLLED = 1, AFX_FLAG_MUSIC = 2, AFX_FLAG_METADATA = 4,
       AFX_FLAG_MUSIC_CHORUS = 8, AFX_FLAG_LANES = 16, AFX_FLAG_BANKS = 64,
       AFX_FLAG_SONG = 128, AFX_FLAG_POOL = 256 };

/* With AFX_FLAG_BANKS, bits 21-22 of a bank-relative sample address select
 * the bank slot that holds the sample; the bits below are the offset in that
 * bank. Slot 0 is the bank of bank_id_low/high; the header's banks_offset
 * lists the identities of slots 1 and up: a u32 count, 1 to 3, then a low and
 * a high u32 word per slot. The image ends with an AFX_BANK_TABLE_BYTES table
 * at the first 4-byte boundary after the stream, zero in the file, which the
 * loader fills with each slot's AICA address (see protocol.h). */
#define AFX_MAX_FLOW_BANKS 4u
#define AFX_BANK_SLOT_SHIFT 21u
#define AFX_BANK_SLOT_OFFSET_MASK ((1u << AFX_BANK_SLOT_SHIFT) - 1u)
#define AFX_BANK_TABLE_BYTES 16u
#define AFX_BANK_TABLE_OFFSET(stream_end) (((stream_end) + 3u) & ~3u)

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
/* AFS is one scene: the banks and flows a program loads together, and where
 * each goes in the one AICA block that holds them. A 32-byte header is
 * followed by entry_count 32-byte entries; names are zero-terminated strings
 * after the entries. A bank entry holds an AFB file and a flow entry an AFX
 * file. A slot entry reserves a range for a bank and a flow loaded later, and
 * a ring entry the DSP delay ring, 16 KiB << RBL at a 2 KiB boundary. */
#define AFX_SCENE_MAGIC 0x00534641u /* "AFS\\0" in little-endian byte order. */
#define AFX_SCENE_VERSION 1u
#define AFX_SCENE_HEADER_BYTES 32u
#define AFX_SCENE_ENTRY_BYTES 32u
#define AFX_SCENE_RING_ALIGN 2048u
#define AFX_SCENE_RING_MIN_BYTES 0x4000u
enum { AFX_SCENE_BANK = 1, AFX_SCENE_FLOW = 2, AFX_SCENE_SLOT = 3, AFX_SCENE_RING = 4,
       AFX_SCENE_SONG = 5 };
/* AFM is a song: sections played in an order (docs/design/song-sections.md).
 * A 96-byte header, the relocations and bank list as in an AFX, then the
 * 32-byte aligned image, uploaded as it is: a 32-byte image header, the
 * setups, the lane map, the section table, the order, the streams and, with
 * AFX_FLAG_BANKS, the bank table after them. A loaded song is followed in
 * AICA RAM by AFX_SONG_RUNTIME_BYTES for its executors when it has a pool. */
#define AFX_SONG_MAGIC 0x314d4641u /* "AFM1" in little-endian byte order. */
#define AFX_SONG_VERSION 1u
#define AFX_SONG_HEADER_BYTES 96u
#define AFX_SONG_IMAGE_HEADER_BYTES 32u
#define AFX_SONG_SECTION_BYTES 12u
#define AFX_SONG_NO_LOOP 0xffffffffu
/* A song's position packs the order position above a stream offset. */
#define AFX_SONG_POSITION_SHIFT 20u
#define AFX_SONG_MAX_ORDER (1u << (32u - AFX_SONG_POSITION_SHIFT))
#define AFX_SONG_MAX_STREAM (1u << AFX_SONG_POSITION_SHIFT)
#define AFX_SONG_POSITION(order, offset) (((uint32_t)(order) << AFX_SONG_POSITION_SHIFT) | (offset))

#define AFX_FILE_HEADER_BYTES 80u
#define AFX_WORK_PROFILE(commands, writes) (((uint32_t)(commands) << 16) | (uint16_t)(writes))
#define AFX_WORK_PROFILE_COMMANDS(profile) ((profile) >> 16)
#define AFX_WORK_PROFILE_WRITES(profile) ((profile) & 0xffffu)
typedef struct {
    uint32_t magic; /* AFX_FILE_MAGIC signature. */
    uint32_t abi; /* AFX file-format version, not the host/firmware ABI. */
    uint32_t total_size; /* Complete AFX file length in bytes. */
    uint32_t flags; /* Authored AFX_FLAG_* options. */
    uint32_t image_offset; /* Upload-image byte offset from the beginning of the file. */
    uint32_t image_size; /* Upload-image length in bytes. */
    uint32_t stream_offset; /* Instruction-stream byte offset relative to the upload image. */
    uint32_t stream_size; /* Instruction-stream length in bytes. */
    uint32_t control_id; /* Identity matching the optional AFC checkpoint sidecar. */
    uint32_t setup_count; /* Number of setup templates at the beginning of the image. */
    uint32_t bank_id_low; /* Low 32 bits of the required AFB identity; both halves zero means no bank. */
    uint32_t bank_id_high; /* High 32 bits of the required AFB identity. */
    uint32_t relocations_offset; /* Binding-table byte offset from the beginning of the file. */
    uint32_t relocation_count; /* Number of 12-byte binding records. */
    uint32_t reserved; /* Zero; was loop_tick before format API 6. */
    uint32_t banks_offset; /* With AFX_FLAG_BANKS, file-relative offset of the bank list; otherwise zero. */
    uint32_t required_channels; /* Local voice count required by the flow, 1..64. */
    uint32_t tick_rate_num; /* Numerator of authored ticks per second. */
    uint32_t tick_rate_den; /* Denominator of authored ticks per second. */
    uint32_t work_profile; /* Packed peak commands/writes; zero requires full loader validation. */
} afx_file_header_t;
typedef struct {
    uint32_t image_offset; /* Sample byte offset relative to an image in the portable sample descriptor. */
    uint32_t byte_size; /* Encoded sample payload length in bytes. */
    uint32_t frames; /* Decoded sample-frame count. */
    uint32_t format; /* AFX_PCM16, AFX_PCM8 or AFX_ADPCM encoding. */
} afx_sample_t;
typedef struct {
    uint32_t pair_offset; /* Image-relative byte offset of a setup CONTROL/SAMPLE_LOW pair. */
    uint32_t sample_index; /* Legacy name: version 7 stores the bank-relative sample byte offset. */
    uint32_t byte_offset; /* Legacy name: version 7 stores the bound sample length in bytes. */
} afx_relocation_t;

typedef struct {
    uint32_t magic; /* AFX_SCENE_MAGIC signature. */
    uint32_t version; /* AFX_SCENE_VERSION. */
    uint32_t id_low; /* Low 32 bits of the scene identity; the identity is nonzero. */
    uint32_t id_high; /* High 32 bits of the scene identity. */
    uint32_t total_size; /* Complete AFS file length in bytes. */
    uint32_t block_bytes; /* Size of the scene's AICA block, a multiple of 32. */
    uint32_t entry_count; /* Number of entries after the header, at least one. */
    uint32_t reserved; /* Zero. */
} afx_scene_header_t;
typedef struct {
    uint32_t kind; /* AFX_SCENE_BANK, AFX_SCENE_FLOW, AFX_SCENE_SLOT or AFX_SCENE_RING. */
    uint32_t name; /* File offset of the entry's zero-terminated name, or zero for none. */
    uint32_t file_offset; /* A bank's AFB or a flow's AFX file; zero for a slot or ring. */
    uint32_t file_size; /* Its length in bytes. */
    uint32_t block_offset; /* Where the entry goes in the block: 32-byte aligned, a ring 2 KiB aligned. */
    uint32_t block_bytes; /* Its range there, a multiple of 32: a bank's payload, a flow's image. */
    uint32_t ticks; /* A flow's authored length in ticks; zero for other kinds. */
    uint32_t reserved; /* Zero. */
} afx_scene_entry_t;

typedef struct {
    uint32_t magic; /* AFX_SONG_MAGIC. */
    uint32_t version; /* AFX_SONG_VERSION. */
    uint32_t total_size; /* Complete AFM file length in bytes. */
    uint32_t flags; /* AFX_FLAG_MUSIC, with AFX_FLAG_LANES and AFX_FLAG_BANKS as in an AFX. */
    uint32_t image_offset; /* Upload image: file offset, a multiple of 32. */
    uint32_t image_size; /* Upload image length in bytes. */
    uint32_t setup_count; /* Setups at image offset AFX_SONG_IMAGE_HEADER_BYTES, one relocation each. */
    uint32_t relocations_offset; /* File offset of setup_count 12-byte relocations, as in an AFX. */
    uint32_t banks_offset; /* With AFX_FLAG_BANKS, the bank list as in an AFX; otherwise zero. */
    uint32_t bank_id_low, bank_id_high; /* Bank slot 0's identity; zero for a song without samples. */
    uint32_t channels; /* Local channels used: one more than the highest any section uses. */
    uint32_t voices; /* The pool a song with a new-play join needs; zero for a continue-only song. */
    uint32_t tick_rate_num, tick_rate_den; /* Authored ticks per second. */
    uint32_t work_profile; /* Peak commands and writes in a tick, both executors together. */
    uint32_t control_id; /* afx_control_id() of the image. */
    uint32_t reserved[7]; /* Zero. */
} afx_song_header_t;
/* The start of a song's image; offsets are relative to the image. */
typedef struct {
    uint32_t section_count, order_count;
    uint32_t loop_index; /* The order position after the last one, or AFX_SONG_NO_LOOP. */
    uint32_t sections_offset; /* section_count entries: stream offset, stream size, body ticks. */
    uint32_t order_offset; /* order_count 16-bit section indices. */
    uint32_t streams_offset, streams_size; /* All streams; the bank table follows them. */
    uint32_t channels; /* Local channels, as in the AFM header; the lane map has as many bytes. */
} afx_song_image_header_t;

/* One AFC checkpoint channel record; also used by runtime rebuild requests. */
typedef struct {
    uint32_t local_channel; /* Local channel index restored by this checkpoint record. */
    uint16_t fields[AFX_FIELD_COUNT]; /* Complete AFX_FIELD_* register words; sample address is bank-relative. */
} afx_checkpoint_channel_t;

#if defined(__cplusplus)
#define AFX_FORMAT_ASSERT static_assert
#else
#define AFX_FORMAT_ASSERT _Static_assert
#endif
AFX_FORMAT_ASSERT(sizeof(afx_file_header_t) == 80, "file header");
AFX_FORMAT_ASSERT(sizeof(afx_sample_t) == 16 && sizeof(afx_relocation_t) == 12, "host tables");
AFX_FORMAT_ASSERT(sizeof(afx_checkpoint_channel_t) == 40, "checkpoint channel");
AFX_FORMAT_ASSERT(sizeof(afx_song_header_t) == AFX_SONG_HEADER_BYTES &&
                  sizeof(afx_song_image_header_t) == AFX_SONG_IMAGE_HEADER_BYTES, "song headers");
AFX_FORMAT_ASSERT(sizeof(afx_scene_header_t) == AFX_SCENE_HEADER_BYTES &&
                  sizeof(afx_scene_entry_t) == AFX_SCENE_ENTRY_BYTES, "scene tables");
#undef AFX_FORMAT_ASSERT
#endif
#endif
