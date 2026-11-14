/* Shared by the shim headers: the SH4 library built for a PC or a browser
 * calls these instead of KOS. Every name has the afx_host_ prefix, so the
 * library defines no KOS symbol. */
#ifndef AICAFLOW_HOST_PLATFORM_SHIM_H
#define AICAFLOW_HOST_PLATFORM_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifndef AFX_HOST_THREADS
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define AFX_HOST_THREADS 0
#else
#define AFX_HOST_THREADS 1
#endif
#endif

uint32_t afx_host_g2_read_32(uintptr_t address);
void afx_host_g2_write_32(uintptr_t address, uint32_t value);
void afx_host_spu_disable(void);
void afx_host_spu_enable(void);
void afx_host_spu_memload(uintptr_t address, const void *source, size_t size);
void afx_host_spu_memset(uintptr_t address, uint32_t value, size_t size);
typedef void (*afx_host_dma_callback_t)(void *data);
int afx_host_spu_dma_transfer(const void *source, uintptr_t address, size_t size, int block,
                              afx_host_dma_callback_t callback, void *data);
typedef void (*afx_host_asic_handler_t)(uint32_t code, void *data);
int afx_host_asic_request_threaded_handler(uint16_t code, afx_host_asic_handler_t handler, void *data,
                                           void (*ack_and_mask)(uint16_t), void (*unmask)(uint16_t));
void afx_host_asic_remove_handler(uint16_t code);
void afx_host_asic_enable(uint16_t code, uint8_t level);
void afx_host_asic_disable(uint16_t code, uint8_t level);
void afx_host_thd_pass(void);
void afx_host_thd_sleep(uint32_t milliseconds);
uint64_t afx_host_timer_ms_gettime64(void);

#endif
