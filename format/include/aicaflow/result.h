#ifndef AICAFLOW_RESULT_H
#define AICAFLOW_RESULT_H
#ifndef __ASSEMBLER__
/* Stable public result numbers shared by the codec and runtime APIs.
 * Runtime-only outcomes stay reserved here so existing numeric results do
 * not depend on which API a consumer includes. No IPC/layout state lives here. */
/* Public status APIs return 0 on success or -AFX_* on failure. */
typedef enum {
    AFX_OK, AFX_BAD_FORMAT, AFX_BAD_BOUNDS, AFX_BAD_COMMAND, AFX_BAD_SAMPLE,
    AFX_BAD_RELOCATION, AFX_NO_AICA_RAM, AFX_NO_HOST_RAM, AFX_NO_CHANNELS,
    AFX_NO_FLOW_SLOTS, AFX_NO_EXEC_BUDGET, AFX_IPC_FULL, AFX_INVALID_HANDLE,
    AFX_STALE_GENERATION, AFX_ASSET_REFERENCED, AFX_BUSY, AFX_UNSUPPORTED,
    AFX_BAD_FIRMWARE, AFX_TIMEOUT
} afx_result_t;
#endif
#endif
