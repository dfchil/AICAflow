#ifndef AICAFLOW_BANK_H
#define AICAFLOW_BANK_H

#include <stdint.h>

#include <aicaflow/host.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AFB is deliberately just an identity and one contiguous sample payload.
 * Sample descriptions belong to the offline authoring tool: an AFX setup has
 * the AICA register values and a bank-relative byte address already. */
#define AFX_BANK_MAGIC 0x00424641u /* "AFB\\0" in little-endian byte order. */
#define AFX_BANK_VERSION 1u
#define AFX_BANK_HEADER_BYTES 32u

/* AFC is a host-only seek index. Its payload is never uploaded to AICA or
 * interpreted by ARM7; it is tied to the exact ABI-7 control_id and AFB id. */
#define AFX_SEEK_MAGIC 0x00434641u /* "AFC\\0" in little-endian byte order. */
#define AFX_SEEK_VERSION 1u
#define AFX_SEEK_HEADER_BYTES 32u

typedef struct {
    uint32_t low, high;
} afx_bank_id_t;

typedef struct {
    afx_asset_t asset;
    afx_bank_id_t id;
    uint32_t bytes;
} afx_bank_t;

/* The input is an AFB file. Its header is always little-endian, even on a
 * host where the C struct representation differs. The complete payload is
 * allocated once in AICA RAM; no sample views or lookup table are created. */
int afx_bank_load_memory(afx_bank_t *bank, const void *data, uint32_t size);
int afx_bank_load_file(afx_bank_t *bank, const char *path);
int afx_bank_release(afx_bank_t *bank);

/* Uploads one sample-free ABI-7 AFX flow and binds all its relocations to
 * bank. A zero bank identity is valid only for a no-sample flow and uses a
 * null bank pointer. The flow retains the bank until it is freed. */
int afx_bank_flow_upload(const afx_bank_t *bank, const void *data, uint32_t size,
                         afx_asset_t *out_flow);

/* Attach an optional AFC seek index before activating flow. Normal playback
 * never needs it. A mismatched flow or bank identity is rejected. */
int afx_flow_seek_index_load_memory(afx_asset_t flow, const void *data, uint32_t size);
int afx_flow_seek_index_load_file(afx_asset_t flow, const char *path);

#ifdef __cplusplus
}
#endif

#endif
