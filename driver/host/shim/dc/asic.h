#ifndef AICAFLOW_HOST_SHIM_ASIC_H
#define AICAFLOW_HOST_SHIM_ASIC_H
#include "../platform_shim.h"
typedef afx_host_asic_handler_t asic_evt_handler;
#define ASIC_EVT_SPU_IRQ 0x0101
#define ASIC_IRQ_DEFAULT 9
#define asic_evt_request_threaded_handler(code, handler, data, ack, unmask) \
    afx_host_asic_request_threaded_handler((code), (handler), (data), (ack), (unmask))
#define asic_evt_remove_handler(code) afx_host_asic_remove_handler(code)
#define asic_evt_enable(code, level) afx_host_asic_enable((code), (level))
#define asic_evt_disable(code, level) afx_host_asic_disable((code), (level))
#endif
