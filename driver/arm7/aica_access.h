/* How the executor reaches AICA RAM and registers. On the ARM7 an AICA address
 * is a pointer and a register is a word at AFX_AICA_REG_BASE + offset. The
 * host backend (driver/host/src/firmware.c) compiles driver.c natively and
 * defines these first, mapping both onto its software AICA. */
#ifndef AICAFLOW_AICA_ACCESS_H
#define AICAFLOW_AICA_ACCESS_H
#include <aicaflow/protocol.h>

/* An AICA RAM address as something to cast to a pointer. */
#ifndef AICA_RAM
#define AICA_RAM(address) ((uintptr_t)(address))
#endif
/* A register write; every one is an action on the chip, in this order. */
#ifndef AICA_REG_WRITE
#define AICA_REG_WRITE(offset, value) (*(volatile uint32_t *)(AFX_AICA_REG_BASE + (offset)) = (value))
#endif
/* A channel's register block: AICA_CHANNEL(regs, physical) declares regs,
 * then AICA_CHANNEL_SET(regs, field, value) writes one field. On the ARM7
 * this is a pointer into the register aperture, as the firmware always had. */
#ifndef AICA_CHANNEL
#define AICA_CHANNEL(name, physical) \
    volatile uint32_t *name = (volatile uint32_t *)(AFX_AICA_REG_BASE + \
        (physical) * AFX_AICA_CHANNEL_REG_STRIDE)
#define AICA_CHANNEL_SET(name, field, value) ((name)[field] = (value))
#endif
/* The linker's layout symbols as AICA addresses. */
#ifndef AICA_ASSET_BASE
extern uint8_t __asset_base[], __private_end[];
#define AICA_ASSET_BASE ((uint32_t)(uintptr_t)__asset_base)
#define AICA_PRIVATE_END ((uint32_t)(uintptr_t)__private_end)
#endif
#endif
