#ifndef AICAFLOW_HOST_SHIM_SPU_H
#define AICAFLOW_HOST_SHIM_SPU_H
#include "../platform_shim.h"
typedef afx_host_dma_callback_t spu_dma_callback_t;
#define spu_disable() afx_host_spu_disable()
#define spu_enable() afx_host_spu_enable()
#define spu_memload(address, source, size) afx_host_spu_memload((address), (source), (size))
#define spu_memset(address, value, size) afx_host_spu_memset((address), (value), (size))
#define spu_dma_transfer(source, address, size, block, callback, data) \
    afx_host_spu_dma_transfer((source), (address), (size), (block), (callback), (data))
#endif
