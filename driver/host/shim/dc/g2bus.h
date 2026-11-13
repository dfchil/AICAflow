#ifndef AICAFLOW_HOST_SHIM_G2BUS_H
#define AICAFLOW_HOST_SHIM_G2BUS_H
#include "../platform_shim.h"
#define g2_read_32(address) afx_host_g2_read_32(address)
#define g2_write_32(address, value) afx_host_g2_write_32((address), (value))
#define g2_read_32_raw(address) afx_host_g2_read_32(address)
#define g2_write_32_raw(address, value) afx_host_g2_write_32((address), (value))
#define g2_lock_scoped() ((void)0)
static inline void g2_fifo_wait(void) {}
#endif
