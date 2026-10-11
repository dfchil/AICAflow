#ifndef AICAFLOW_LIMITS_H
#define AICAFLOW_LIMITS_H
/* AICA target acceptance limits, not physical RAM addresses. Actual available
 * memory is lower when firmware, other banks or DSP delay RAM are resident. */
#define AFX_TARGET_MAX_BANK_BYTES 0x1fcec0
/* Maximum work an authoring tool may place in one flow's 1 ms executor
 * interval. The runtime does not enforce it: the executor runs late instead.
 * Keep the existing hardware-calibrated limits when updating the contract. */
#define AFX_EXECUTION_BUDGET_COMMANDS 38u
#define AFX_EXECUTION_BUDGET_WRITES 171u
#endif
