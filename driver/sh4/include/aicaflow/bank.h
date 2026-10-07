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
/* AFC is a host-only seek index. Its payload is never uploaded to AICA or
 * interpreted by ARM7; it is tied to the exact ABI-7 control_id and AFB id. */
typedef struct {
    uint32_t low; /* Low 32 bits of the authored 64-bit bank identity. */
    uint32_t high; /* High 32 bits of the authored 64-bit bank identity. */
} afx_bank_id_t;

typedef struct {
    afx_asset_t asset; /* Handle owning the uploaded AFB payload, or AFX_ASSET_INVALID. */
    afx_bank_id_t id; /* Authored bank identity matched against dependent AFX/AFC files. */
    uint32_t bytes; /* Logical payload length in bytes, excluding the file header. */
} afx_bank_t;

/* The input is an AFB file. Its header is always little-endian, even on a
 * host where the C struct representation differs. The complete payload is
 * allocated once in AICA RAM; no sample views or lookup table are created. */
int afx_bank_load_memory(afx_bank_t *bank, const void *data, uint32_t size);
int afx_bank_load_file(afx_bank_t *bank, const char *path);
int afx_bank_release(afx_bank_t *bank);

/* Uploads one sample-free version-7 AFX flow unchanged and validates sample
 * ranges against bank. A zero bank identity is valid only for a no-sample flow and uses a
 * null bank pointer. The flow retains the bank until it is freed, but retains
 * no SH-4 copy of the image. The caller may release data after this returns. */
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
