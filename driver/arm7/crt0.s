#include "aicaflow/protocol.h"
.section .vectors, "ax"
.global reset
reset:
    b start
    b exception
    b exception
    b exception
    b exception
    b exception
    b exception
    b fiq

.text
start:
    /* Establish EVERY exception stack before C; no inherited loader state. */
    /* Flycast's ARM recompiler accepts register CPSR_cf writes, but silently
     * drops CPSR_c writes. Startup flags are unused; enable preserves them. */
    mov r0, #0xdb
    msr cpsr_cf, r0
    ldr sp, =AFX_UND_STACK_TOP
    mov r0, #0xd7
    msr cpsr_cf, r0
    ldr sp, =AFX_ABT_STACK_TOP
    mov r0, #0xd2
    msr cpsr_cf, r0
    ldr sp, =AFX_IRQ_STACK_TOP
    mov r0, #0xd1
    msr cpsr_cf, r0
    ldr sp, =AFX_FIQ_STACK_TOP
    mov r0, #0xd3
    msr cpsr_cf, r0
    ldr sp, =AFX_SVC_STACK_TOP

    ldr r0, =AFX_STACK_BASE
    ldr r1, =AFX_CLOCK_BASE
    ldr r2, =AFX_STACK_PATTERN
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b

    ldr r0, =__data_load
    ldr r1, =__data_start
    ldr r2, =__data_end
2:  cmp r1, r2
    ldrlo r3, [r0], #4
    strlo r3, [r1], #4
    blo 2b
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
3:  cmp r0, r1
    strlo r2, [r0], #4
    blo 3b
    bl arm_main
exception:
    /* Stage 1 has no interrupt/executor capability. Fail closed on exceptions. */
    ldr r0, =AFX_STATUS_ADDR
    mov r1, #1
    str r1, [r0, #40]
4:  b 4b

/* FIQ uses only r8/r9, which are banked in FIQ mode. Timer A only advances the
 * shared clock and reloads itself; decoding and hardware writes remain in C. */
fiq:
    ldr r8, =0x00802d00
    ldr r9, [r8]
    and r9, r9, #7
    cmp r9, #2
    beq fiq_timer
    cmp r9, #5
    bne fiq_done
    ldr r8, =0x00802808
1:  ldr r9, [r8]
    tst r9, #0x100
    bne 1b
    b fiq_done
fiq_timer:
    ldr r8, =AFX_AICA_TIMER_TICK_ADDR
    ldr r9, [r8]
    add r9, r9, #1
    str r9, [r8]
    ldr r8, =0x00802880
    ldr r9, =AFX_TIMER_RELOAD
    str r9, [r8, #0x10]
    mov r9, #0x40
    str r9, [r8, #0x24]
fiq_done:
    ldr r8, =0x00802d04
    mov r9, #1
    str r9, [r8]
    str r9, [r8]
    str r9, [r8]
    str r9, [r8]
    subs pc, r14, #4

.global arm_fiq_enable
arm_fiq_enable:
    mrs r0, cpsr
    orr r0, r0, #0x80
    bic r0, r0, #0x40
    msr cpsr_cf, r0
    mov pc, lr
