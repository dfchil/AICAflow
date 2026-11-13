/* An interpreter for the AICA's ARM7DI: the ARM instruction set without
 * Thumb, enough to run the AICAflow firmware image. */
#ifndef AICAFLOW_HOST_ARM7_H
#define AICAFLOW_HOST_ARM7_H

#include <stdbool.h>
#include <stdint.h>

enum {
    ARM7_MODE_USR = 0x10, ARM7_MODE_FIQ = 0x11, ARM7_MODE_IRQ = 0x12,
    ARM7_MODE_SVC = 0x13, ARM7_MODE_ABT = 0x17, ARM7_MODE_UND = 0x1b,
    ARM7_MODE_SYS = 0x1f
};
#define ARM7_FLAG_N 0x80000000u
#define ARM7_FLAG_Z 0x40000000u
#define ARM7_FLAG_C 0x20000000u
#define ARM7_FLAG_V 0x10000000u
#define ARM7_FLAG_I 0x80u
#define ARM7_FLAG_F 0x40u

typedef struct arm7 {
    uint32_t r[16];
    uint32_t cpsr;
    /* Banked registers of the modes not current. bank[0] is user/system. */
    uint32_t banked_r13[6], banked_r14[6], spsr[6];
    uint32_t user_r8_12[5], fiq_r8_12[5];
    bool fiq_line; /* Asserted by the AICA while an enabled interrupt is pending. */
    bool halted;   /* Set by an undecodable state the firmware cannot leave. */
    uint64_t instructions;
    void *bus;
    uint32_t (*read32)(void *bus, uint32_t address);
    uint8_t (*read8)(void *bus, uint32_t address);
    void (*write32)(void *bus, uint32_t address, uint32_t value);
    void (*write8)(void *bus, uint32_t address, uint8_t value);
} arm7_t;

/* Reset: SVC mode, IRQ and FIQ disabled, PC at the reset vector. */
void arm7_reset(arm7_t *cpu);
/* Executes up to count instructions, taking the FIQ between them. */
void arm7_run(arm7_t *cpu, uint32_t count);

#endif
