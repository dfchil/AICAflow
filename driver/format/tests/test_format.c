/* Deliberately compiled with only driver/format/include, not driver/include. */
#include <aicaflow/format.h>
#include <aicaflow/limits.h>
#include <aicaflow/result.h>

#if defined(AFX_ABI_VERSION) || defined(AFX_CONTROL_BASE) || defined(AFX_CMD_QUEUE_CAPACITY)
#error "The public asset format must not expose firmware or IPC internals"
#endif
#if AFX_FORMAT_API_VERSION != 6 || AFX_FILE_MAGIC != 0x32584641
#error "Unexpected public format API or AFX magic"
#endif

#ifndef __ASSEMBLER__
#include <assert.h>
#include <stddef.h>

int main(void) {
    assert(AFX_FILE_VERSION == 7 && AFX_BANK_VERSION == 1);
    assert(AFX_SEEK_VERSION == 1 && AFX_INDEX_VERSION == 1);
    assert(AFX_CHECKPOINT_VERSION == 1 && AFX_MAX_FLOW_CHANNELS == 64);
    assert(sizeof(afx_checkpoint_channel_t) == 40);
    assert(AFX_TARGET_MAX_BANK_BYTES == 0x1fcec0);
    assert(AFX_EXECUTION_BUDGET_COMMANDS == 38 && AFX_EXECUTION_BUDGET_WRITES == 171);
    assert(AFX_BAD_RELOCATION == 5 && AFX_BAD_FIRMWARE == 17 && AFX_TIMEOUT == 18);
    assert(AFX_FILE_HEADER_BYTES == 80 && sizeof(afx_file_header_t) == 80);
    assert(AFX_BANK_HEADER_BYTES == 32 && AFX_SEEK_HEADER_BYTES == 32);
    assert(AFX_INDEX_HEADER_BYTES == 32 && AFX_INDEX_RECORD_BYTES == 16);
    assert(AFX_INDEX_NAMED_RECORD_BYTES == 32);
    assert(offsetof(afx_file_header_t, control_id) == 32);
    assert(offsetof(afx_file_header_t, bank_id_low) == 40);
    assert(offsetof(afx_file_header_t, work_profile) == 76);
    assert(sizeof(afx_sample_t) == 16 && sizeof(afx_relocation_t) == 12);
    assert(AFX_FIELD_COUNT == 18 && AFX_SETUP_BYTES == 36);
    assert(AFX_FIELD_DSP_SEND == 8 && AFX_FIELD_DIRECT == 9 && AFX_FIELD_MIX == 10);
    assert(AFX_OP_NOTE == 0x10 && AFX_OP_NOTE_PL == 0x14 && AFX_OP_PATCH_LEVEL == 0x15);
    assert(AFX_OP_PATCH_ENV == 0x17 && AFX_OP_PATCH_ENV_LEVEL == 0x18);
    assert(AFX_OP_NEXT == 0x19 && AFX_OP_FREE == 0x1a && AFX_JOIN_NEW == 0 && AFX_JOIN_CONTINUE == 1);
    assert(AFX_FLAG_CONTROLLED == 1 && AFX_FLAG_MUSIC == 2 && AFX_FLAG_LANES == 16);
    assert(AFX_FLAG_SONG == 128 && AFX_FLAG_POOL == 256 && offsetof(afx_file_header_t, reserved) == 56);
    assert(AFX_SONG_MAGIC == 0x314d4641u && sizeof(afx_song_header_t) == 96 && AFX_SCENE_SONG == 5);
    assert(AFX_SONG_POSITION(3, 5) == ((3u << 20) | 5u));
    assert(AFX_FLAG_BANKS == 64 && offsetof(afx_file_header_t, banks_offset) == 60);
    assert(AFX_MAX_FLOW_BANKS == 4 && AFX_BANK_SLOT_SHIFT == 21 && AFX_BANK_TABLE_BYTES == 16);
    assert(AFX_BANK_TABLE_OFFSET(37u) == 40 && AFX_BANK_TABLE_OFFSET(40u) == 40);
    assert(AFX_PCM16 == 0 && AFX_PCM8 == 1 && AFX_ADPCM == 2);
    assert(AFX_SCENE_MAGIC == 0x00534641u && AFX_SCENE_VERSION == 1);
    assert(sizeof(afx_scene_header_t) == 32 && sizeof(afx_scene_entry_t) == 32);
    assert(offsetof(afx_scene_header_t, block_bytes) == 20 && offsetof(afx_scene_entry_t, ticks) == 24);
    assert(AFX_SCENE_BANK == 1 && AFX_SCENE_FLOW == 2 && AFX_SCENE_SLOT == 3 && AFX_SCENE_RING == 4);
    assert(AFX_SCENE_RING_ALIGN == 2048 && AFX_SCENE_RING_MIN_BYTES == 0x4000);
    assert(AFX_WORK_PROFILE_COMMANDS(AFX_WORK_PROFILE(38, 171)) == 38);
    assert(AFX_WORK_PROFILE_WRITES(AFX_WORK_PROFILE(38, 171)) == 171);
    return 0;
}
#endif
