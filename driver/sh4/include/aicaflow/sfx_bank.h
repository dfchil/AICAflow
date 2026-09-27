#ifndef AICAFLOW_SFX_BANK_H
#define AICAFLOW_SFX_BANK_H

#include <stdint.h>

#include <aicaflow/host.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AFB1 groups independently activatable SFX flows around shared samples. */
#define AFX_SFX_BANK_VERSION 1u
#define AFX_SFX_BANK_CONTROLLED 1u

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version, sound_count;
    uint32_t sample_count, sample_offset, sound_offset, setup_offset;
    uint32_t stream_offset, data_offset, size;
} afx_sfx_bank_header_t;

typedef struct __attribute__((packed)) {
    uint32_t offset, bytes, frames, format;
} afx_sfx_bank_sample_t;

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint8_t channels, flags;
    uint16_t setup_first, setup_count;
    uint32_t stream_offset, stream_size;
} afx_sfx_bank_sound_t;

typedef struct __attribute__((packed)) {
    uint16_t sample, reserved;
    uint32_t byte_offset;
    uint16_t fields[AFX_FIELD_COUNT];
} afx_sfx_bank_setup_t;

/* AFC1 is one control stream whose setups refer to AFB1 sample indices. */
typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint8_t channels, flags;
    uint16_t setup_count;
    uint32_t stream_size, size;
} afx_sfx_control_header_t;

typedef struct {
    afx_asset_t sample_data;
    afx_asset_t *samples;
    afx_asset_t *flows;
    afx_sfx_bank_header_t *header;
    afx_sfx_bank_sound_t *sounds;
    afx_sfx_bank_setup_t *setups;
    char *path;
    uint32_t bytes;
} afx_sfx_bank_t;

_Static_assert(sizeof(afx_sfx_bank_header_t) == 36 && sizeof(afx_sfx_bank_sample_t) == 16 &&
               sizeof(afx_sfx_bank_sound_t) == 16 && sizeof(afx_sfx_bank_setup_t) == 44 &&
               sizeof(afx_sfx_control_header_t) == 18,
               "AFB1 layout changed");

/* Reads, validates and uploads an AFB1 file. Samples and flows remain valid
 * until afx_sfx_bank_release(). The loader keeps only its compact tables on SH-4. */
int afx_sfx_bank_load_file(afx_sfx_bank_t *bank, const char *path);
/* Loads and retains only the shared samples and compact tables. Upload one
 * member later with afx_sfx_bank_flow_upload(); the caller owns that flow. */
int afx_sfx_bank_load_samples_file(afx_sfx_bank_t *bank, const char *path);
int afx_sfx_bank_flow_upload(const afx_sfx_bank_t *bank, uint32_t index,
                             afx_asset_t *out_flow);
/* Upload one AFC1 control stream using samples retained by an AFB1 bank. */
int afx_sfx_bank_control_upload(const afx_sfx_bank_t *bank, const char *path,
                                afx_asset_t *out_flow);
int afx_sfx_bank_release(afx_sfx_bank_t *bank);

#ifdef __cplusplus
}
#endif

#endif
