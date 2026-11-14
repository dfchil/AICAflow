#ifndef AICAFLOW_HOST_SHIM_CACHE_H
#define AICAFLOW_HOST_SHIM_CACHE_H
#include <stdint.h>
/* Host memory is coherent: DMA is a memcpy. */
#define dcache_wback_range(address, size) ((void)(address), (void)(size))
#endif
