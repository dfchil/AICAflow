/* The KOS calls of the console checks (driver/tests/check_*.c), for host
 * runs: time is the audio rendered, so a sleep renders that much. */
#ifndef AICAFLOW_HOST_TEST_KOS_H
#define AICAFLOW_HOST_TEST_KOS_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint32_t afx_host_g2_read_32(uintptr_t address);
#define g2_read_32(address) afx_host_g2_read_32(address)
uint64_t afx_test_ms(void);
uint64_t afx_test_us(void);
void afx_test_sleep(uint32_t milliseconds);
void afx_test_pass(void);
#define timer_ms_gettime64() afx_test_ms()
#define timer_us_gettime64() afx_test_us()
#define thd_sleep(milliseconds) afx_test_sleep(milliseconds)
#define thd_pass() afx_test_pass()
#endif
