/* ARM7 interpreter cases: each runs a few instructions from a small RAM. */
#include "arm7.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[4096];
static uint32_t r32(void *bus, uint32_t a) { (void)bus; a &= sizeof(ram) - 4; return ram[a] | ram[a + 1] << 8 | ram[a + 2] << 16 | (uint32_t)ram[a + 3] << 24; }
static uint8_t r8(void *bus, uint32_t a) { (void)bus; return ram[a & (sizeof(ram) - 1)]; }
static void w32(void *bus, uint32_t a, uint32_t v) { (void)bus; a &= sizeof(ram) - 4; ram[a] = (uint8_t)v; ram[a + 1] = (uint8_t)(v >> 8); ram[a + 2] = (uint8_t)(v >> 16); ram[a + 3] = (uint8_t)(v >> 24); }
static void w8(void *bus, uint32_t a, uint8_t v) { (void)bus; ram[a & (sizeof(ram) - 1)] = v; }

static arm7_t run(const uint32_t *code, unsigned count, unsigned steps) {
    arm7_t cpu = { .read32 = r32, .read8 = r8, .write32 = w32, .write8 = w8 };
    memset(ram, 0, sizeof(ram));
    for (unsigned i = 0; i < count; ++i) w32(NULL, i * 4, code[i]);
    arm7_reset(&cpu);
    arm7_run(&cpu, steps);
    return cpu;
}

int main(void) {
    /* mov r0,#5; mov r1,#3; subs r2,r0,r1; rsb r3,r0,#0 */
    static const uint32_t arith[] = {0xe3a00005, 0xe3a01003, 0xe0502001, 0xe2603000};
    arm7_t c = run(arith, 4, 4);
    assert(c.r[2] == 2 && (c.cpsr & ARM7_FLAG_C) && c.r[3] == (uint32_t)-5);
    /* mvn r0,#0; adds r1,r0,#1 -> 0 with carry and Z */
    static const uint32_t carry[] = {0xe3e00000, 0xe2901001};
    c = run(carry, 2, 2);
    assert(c.r[1] == 0 && (c.cpsr & ARM7_FLAG_Z) && (c.cpsr & ARM7_FLAG_C) && !(c.cpsr & ARM7_FLAG_V));
    /* mov r0,#0x80000000 (0xe3a00102); adds r1,r0,r0 -> overflow */
    static const uint32_t overflow[] = {0xe3a00102, 0xe0901000};
    c = run(overflow, 2, 2);
    assert(c.r[1] == 0 && (c.cpsr & ARM7_FLAG_V) && (c.cpsr & ARM7_FLAG_C));
    /* mov r0,#0x100; mov r1,#0xab; str r1,[r0,#4]!; ldrb r2,[r0]; ldr r3,[r0],#-4 */
    static const uint32_t memory[] = {0xe3a00c01, 0xe3a010ab, 0xe5a01004, 0xe5d02000, 0xe4103004};
    c = run(memory, 5, 5);
    assert(c.r[2] == 0xab && c.r[3] == 0xab && c.r[0] == 0x100 && r32(NULL, 0x104) == 0xab);
    /* mov r0,#0x200; mov r1,#1; mov r2,#2; stmdb r0!,{r1,r2}; ldmia r0,{r3,r4} */
    static const uint32_t block[] = {0xe3a00c02, 0xe3a01001, 0xe3a02002, 0xe9200006, 0xe8900018};
    c = run(block, 5, 5);
    assert(c.r[0] == 0x1f8 && c.r[3] == 1 && c.r[4] == 2);
    /* bl +8 (to 0x0c); mov r0,#1; mov r1,#1; mov r2,#7; mov pc,lr */
    static const uint32_t branch[] = {0xeb000001, 0xe3a00001, 0xe3a01001, 0xe3a02007, 0xe1a0f00e};
    c = run(branch, 5, 3);
    assert(c.r[2] == 7 && c.r[14] == 4 && c.r[15] == 4 && c.r[0] == 0);
    /* mov r0,#3; mov r1,#7; mul r2,r0,r1; mla r3,r0,r1,r2; umull r4,r5,r1,r1 */
    static const uint32_t multiply[] = {0xe3a00003, 0xe3a01007, 0xe0020190, 0xe0232190, 0xe0854191};
    c = run(multiply, 5, 5);
    assert(c.r[2] == 21 && c.r[3] == 42 && c.r[4] == 49 && c.r[5] == 0);
    /* Shifts: mov r0,#1; mov r1,r0,lsl #31; movs r2,r1,lsr #31 (carry 0); movs r3,r1,asr #31 */
    static const uint32_t shifts[] = {0xe3a00001, 0xe1a01f80, 0xe1b02fa1, 0xe1b03fc1};
    c = run(shifts, 4, 4);
    assert(c.r[1] == 0x80000000u && c.r[2] == 1 && c.r[3] == 0xffffffffu);
    /* Modes: msr cpsr_c,#0xd1 (FIQ) ; mov r8,#9 ; msr cpsr_c,#0xd3 (SVC) -> r8 user is 0 */
    static const uint32_t modes[] = {0xe321f0d1, 0xe3a08009, 0xe321f0d3};
    c = run(modes, 3, 3);
    assert((c.cpsr & 0x1f) == ARM7_MODE_SVC && c.r[8] == 0 && c.fiq_r8_12[0] == 9);
    /* The FIQ: enable it, raise the line, and return with subs pc,lr,#4.
     * 0x00: b 0x40 ; 0x1c: subs pc,lr,#4 (handler) ; 0x40: mrs r0,cpsr ; bic r0,r0,#0x40 ;
     * msr cpsr_cf,r0 ; mov r1,#1 ; mov r2,#2 */
    static const uint32_t fiq_code[0x60 / 4] = {
        [0] = 0xea00000e, [7] = 0xe25ef004,
        [16] = 0xe10f0000, [17] = 0xe3c00040, [18] = 0xe129f000, [19] = 0xe3a01001, [20] = 0xe3a02002};
    arm7_t f = { .read32 = r32, .read8 = r8, .write32 = w32, .write8 = w8 };
    memset(ram, 0, sizeof(ram));
    for (unsigned i = 0; i < sizeof(fiq_code) / 4; ++i) w32(NULL, i * 4, fiq_code[i]);
    arm7_reset(&f);
    arm7_run(&f, 4);           /* b, mrs, bic, msr: FIQ now enabled */
    f.fiq_line = true;
    arm7_run(&f, 1);           /* takes the FIQ, runs subs pc,lr,#4 */
    assert((f.cpsr & 0x1f) == ARM7_MODE_FIQ || (f.cpsr & 0x1f) == ARM7_MODE_SVC);
    f.fiq_line = false;
    arm7_run(&f, 3);
    assert((f.cpsr & 0x1f) == ARM7_MODE_SVC && f.r[1] == 1 && f.r[2] == 2);
    puts("arm7 interpreter checks passed");
    return 0;
}
