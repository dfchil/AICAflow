#ifndef AICAFLOW_HOST_SHIM_THREAD_H
#define AICAFLOW_HOST_SHIM_THREAD_H
#include "../platform_shim.h"
#define thd_pass() afx_host_thd_pass()
#define thd_sleep(milliseconds) afx_host_thd_sleep(milliseconds)
#endif
