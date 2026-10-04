#ifndef AICAFLOW_LIMITS_H
#define AICAFLOW_LIMITS_H
/* AICA target acceptance limits, not physical RAM addresses. Actual available
 * memory is lower when firmware, other banks or DSP delay RAM are resident. */
#define AFX_TARGET_MAX_BANK_BYTES 0x1fc000
/* Maximum authored work per 1 ms executor interval. Keep the existing
 * hardware-calibrated limits when updating the public target contract. */
#define AFX_EXECUTION_BUDGET_COMMANDS 38u
#define AFX_EXECUTION_BUDGET_WRITES 171u
#endif
