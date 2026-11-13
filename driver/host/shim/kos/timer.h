#ifndef AICAFLOW_HOST_SHIM_TIMER_H
#define AICAFLOW_HOST_SHIM_TIMER_H
#include "../platform_shim.h"
#define timer_ms_gettime64() afx_host_timer_ms_gettime64()
#endif
