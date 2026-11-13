#include "arm7.h"

#include <string.h>

/* Index of a mode's banked registers; user and system share index 0. */
static int bank_index(uint32_t mode) {
    switch (mode & 0x1f) {
    case ARM7_MODE_FIQ: return 1;
    case ARM7_MODE_IRQ: return 2;
    case ARM7_MODE_SVC: return 3;
    case ARM7_MODE_ABT: return 4;
    case ARM7_MODE_UND: return 5;
    default: return 0;
    }
}

static void switch_mode(arm7_t *cpu, uint32_t mode) {
    uint32_t old = cpu->cpsr & 0x1f;
    if ((mode & 0x1f) == old) return;
    int from = bank_index(old), to = bank_index(mode);
    cpu->banked_r13[from] = cpu->r[13];
    cpu->banked_r14[from] = cpu->r[14];
    if (from == 1 && to != 1) {
        memcpy(cpu->fiq_r8_12, &cpu->r[8], sizeof(cpu->fiq_r8_12));
        memcpy(&cpu->r[8], cpu->user_r8_12, sizeof(cpu->user_r8_12));
    } else if (to == 1 && from != 1) {
        memcpy(cpu->user_r8_12, &cpu->r[8], sizeof(cpu->user_r8_12));
        memcpy(&cpu->r[8], cpu->fiq_r8_12, sizeof(cpu->fiq_r8_12));
    }
    cpu->r[13] = cpu->banked_r13[to];
    cpu->r[14] = cpu->banked_r14[to];
    cpu->cpsr = (cpu->cpsr & ~0x1fu) | (mode & 0x1f);
}

static void set_cpsr(arm7_t *cpu, uint32_t value) {
    switch_mode(cpu, value & 0x1f);
    cpu->cpsr = value;
}

static uint32_t *spsr(arm7_t *cpu) {
    int index = bank_index(cpu->cpsr);
    return index ? &cpu->spsr[index] : NULL;
}

void arm7_reset(arm7_t *cpu) {
    void *bus = cpu->bus;
    uint32_t (*read32)(void *, uint32_t) = cpu->read32;
    uint8_t (*read8)(void *, uint32_t) = cpu->read8;
    void (*write32)(void *, uint32_t, uint32_t) = cpu->write32;
    void (*write8)(void *, uint32_t, uint8_t) = cpu->write8;
    memset(cpu, 0, sizeof(*cpu));
    cpu->bus = bus;
    cpu->read32 = read32;
    cpu->read8 = read8;
    cpu->write32 = write32;
    cpu->write8 = write8;
    cpu->cpsr = ARM7_MODE_SVC | ARM7_FLAG_I | ARM7_FLAG_F;
}

static void exception(arm7_t *cpu, uint32_t mode, uint32_t vector, uint32_t link, bool fiq) {
    uint32_t saved = cpu->cpsr;
    switch_mode(cpu, mode);
    cpu->spsr[bank_index(mode)] = saved;
    cpu->r[14] = link;
    cpu->cpsr |= ARM7_FLAG_I | (fiq ? ARM7_FLAG_F : 0);
    cpu->r[15] = vector;
}

static bool condition(uint32_t cond, uint32_t cpsr) {
    bool n = cpsr & ARM7_FLAG_N, z = cpsr & ARM7_FLAG_Z;
    bool c = cpsr & ARM7_FLAG_C, v = cpsr & ARM7_FLAG_V;
    switch (cond) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xa: return n == v;
    case 0xb: return n != v;
    case 0xc: return !z && n == v;
    case 0xd: return z || n != v;
    case 0xe: return true;
    default: return false; /* 0xf is unpredictable on ARMv3/v4. */
    }
}

static uint32_t ror32(uint32_t value, uint32_t amount) {
    amount &= 31;
    return amount ? (value >> amount) | (value << (32 - amount)) : value;
}

/* r[15] holds the address of the next instruction while one executes, so the
 * architectural PC (instruction + 8) is r[15] + 4. */
static uint32_t reg(const arm7_t *cpu, uint32_t n) {
    return n == 15 ? cpu->r[15] + 4 : cpu->r[n];
}

static void set_nz(arm7_t *cpu, uint32_t value) {
    cpu->cpsr = (cpu->cpsr & ~(ARM7_FLAG_N | ARM7_FLAG_Z)) |
                (value & ARM7_FLAG_N) | (value ? 0 : ARM7_FLAG_Z);
}

static void set_c(arm7_t *cpu, bool carry) {
    cpu->cpsr = carry ? cpu->cpsr | ARM7_FLAG_C : cpu->cpsr & ~ARM7_FLAG_C;
}

static void set_v(arm7_t *cpu, bool overflow) {
    cpu->cpsr = overflow ? cpu->cpsr | ARM7_FLAG_V : cpu->cpsr & ~ARM7_FLAG_V;
}

/* The barrel shifter for a register operand. */
static uint32_t shifted_register(arm7_t *cpu, uint32_t op, bool *carry) {
    uint32_t rm = op & 15, type = (op >> 5) & 3, value, amount;
    *carry = (cpu->cpsr & ARM7_FLAG_C) != 0;
    if (op & 0x10) {
        /* Register-specified amount: the PC reads one instruction further. */
        value = rm == 15 ? cpu->r[15] + 8 : cpu->r[rm];
        amount = cpu->r[(op >> 8) & 15] & 0xff;
        if (!amount) return value;
        switch (type) {
        case 0:
            if (amount < 32) { *carry = (value >> (32 - amount)) & 1; return value << amount; }
            *carry = amount == 32 ? value & 1 : 0;
            return 0;
        case 1:
            if (amount < 32) { *carry = (value >> (amount - 1)) & 1; return value >> amount; }
            *carry = amount == 32 ? value >> 31 : 0;
            return 0;
        case 2:
            if (amount < 32) { *carry = (value >> (amount - 1)) & 1; return (uint32_t)((int32_t)value >> amount); }
            *carry = value >> 31;
            return (uint32_t)((int32_t)value >> 31);
        default:
            if (!(amount & 31)) { *carry = value >> 31; return value; }
            *carry = (value >> ((amount & 31) - 1)) & 1;
            return ror32(value, amount);
        }
    }
    value = reg(cpu, rm);
    amount = (op >> 7) & 31;
    switch (type) {
    case 0:
        if (!amount) return value;
        *carry = (value >> (32 - amount)) & 1;
        return value << amount;
    case 1:
        if (!amount) { *carry = value >> 31; return 0; }
        *carry = (value >> (amount - 1)) & 1;
        return value >> amount;
    case 2:
        if (!amount) { *carry = value >> 31; return (uint32_t)((int32_t)value >> 31); }
        *carry = (value >> (amount - 1)) & 1;
        return (uint32_t)((int32_t)value >> amount);
    default:
        if (!amount) { /* RRX */
            bool old = (cpu->cpsr & ARM7_FLAG_C) != 0;
            *carry = value & 1;
            return (value >> 1) | ((uint32_t)old << 31);
        }
        *carry = (value >> (amount - 1)) & 1;
        return ror32(value, amount);
    }
}

static uint32_t load_word(arm7_t *cpu, uint32_t address) {
    return ror32(cpu->read32(cpu->bus, address & ~3u), (address & 3u) * 8u);
}

static void write_pc(arm7_t *cpu, uint32_t value) { cpu->r[15] = value & ~3u; }

static void data_processing(arm7_t *cpu, uint32_t op) {
    uint32_t opcode = (op >> 21) & 15, rd = (op >> 12) & 15, rn = (op >> 16) & 15;
    bool s = (op >> 20) & 1, carry;
    uint32_t operand;
    if (op & (1u << 25)) {
        uint32_t rotate = ((op >> 8) & 15) * 2;
        operand = ror32(op & 0xff, rotate);
        carry = rotate ? operand >> 31 : (cpu->cpsr & ARM7_FLAG_C) != 0;
    } else {
        operand = shifted_register(cpu, op, &carry);
    }
    uint32_t a = (!(op & (1u << 25)) && (op & 0x10) && rn == 15) ? cpu->r[15] + 8 : reg(cpu, rn);
    uint32_t result = 0;
    bool logical = false, write = true;
    uint64_t wide;
    bool c_in = (cpu->cpsr & ARM7_FLAG_C) != 0;
    bool c_out = false, v_out = false;
    switch (opcode) {
    case 0x0: result = a & operand; logical = true; break;
    case 0x1: result = a ^ operand; logical = true; break;
    case 0x2: result = a - operand; c_out = a >= operand; v_out = ((a ^ operand) & (a ^ result)) >> 31; break;
    case 0x3: result = operand - a; c_out = operand >= a; v_out = ((operand ^ a) & (operand ^ result)) >> 31; break;
    case 0x4: wide = (uint64_t)a + operand; result = (uint32_t)wide; c_out = wide >> 32; v_out = (~(a ^ operand) & (a ^ result)) >> 31; break;
    case 0x5: wide = (uint64_t)a + operand + c_in; result = (uint32_t)wide; c_out = wide >> 32; v_out = (~(a ^ operand) & (a ^ result)) >> 31; break;
    case 0x6: wide = (uint64_t)a - operand - !c_in; result = (uint32_t)wide; c_out = !(wide >> 32); v_out = ((a ^ operand) & (a ^ result)) >> 31; break;
    case 0x7: wide = (uint64_t)operand - a - !c_in; result = (uint32_t)wide; c_out = !(wide >> 32); v_out = ((operand ^ a) & (operand ^ result)) >> 31; break;
    case 0x8: result = a & operand; logical = true; write = false; break;
    case 0x9: result = a ^ operand; logical = true; write = false; break;
    case 0xa: result = a - operand; c_out = a >= operand; v_out = ((a ^ operand) & (a ^ result)) >> 31; write = false; break;
    case 0xb: wide = (uint64_t)a + operand; result = (uint32_t)wide; c_out = wide >> 32; v_out = (~(a ^ operand) & (a ^ result)) >> 31; write = false; break;
    case 0xc: result = a | operand; logical = true; break;
    case 0xd: result = operand; logical = true; break;
    case 0xe: result = a & ~operand; logical = true; break;
    default: result = ~operand; logical = true; break;
    }
    if (write) {
        if (rd == 15) {
            write_pc(cpu, result);
            if (s) {
                uint32_t *saved = spsr(cpu);
                if (saved) set_cpsr(cpu, *saved);
            }
            return;
        }
        cpu->r[rd] = result;
    }
    if (s) {
        set_nz(cpu, result);
        if (logical) set_c(cpu, carry);
        else { set_c(cpu, c_out); set_v(cpu, v_out); }
    }
}

static void psr_transfer(arm7_t *cpu, uint32_t op) {
    bool use_spsr = (op >> 22) & 1;
    if (!(op & (1u << 21))) { /* MRS */
        uint32_t *saved = spsr(cpu);
        cpu->r[(op >> 12) & 15] = use_spsr ? (saved ? *saved : cpu->cpsr) : cpu->cpsr;
        return;
    }
    uint32_t value = (op & (1u << 25)) ? ror32(op & 0xff, ((op >> 8) & 15) * 2) : cpu->r[op & 15];
    uint32_t mask = 0;
    if (op & (1u << 19)) mask |= 0xff000000u;
    if (op & (1u << 18)) mask |= 0x00ff0000u;
    if (op & (1u << 17)) mask |= 0x0000ff00u;
    if (op & (1u << 16)) mask |= 0x000000ffu;
    if (use_spsr) {
        uint32_t *saved = spsr(cpu);
        if (saved) *saved = (*saved & ~mask) | (value & mask);
        return;
    }
    if ((cpu->cpsr & 0x1f) == ARM7_MODE_USR) mask &= 0xff000000u;
    set_cpsr(cpu, (cpu->cpsr & ~mask) | (value & mask));
}

static void multiply(arm7_t *cpu, uint32_t op) {
    uint32_t rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    uint32_t result = cpu->r[rm] * cpu->r[rs];
    if (op & (1u << 21)) result += cpu->r[rn];
    cpu->r[rd] = result;
    if (op & (1u << 20)) set_nz(cpu, result);
}

static void multiply_long(arm7_t *cpu, uint32_t op) {
    uint32_t hi = (op >> 16) & 15, lo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    uint64_t result;
    if (op & (1u << 22)) result = (uint64_t)((int64_t)(int32_t)cpu->r[rm] * (int32_t)cpu->r[rs]);
    else result = (uint64_t)cpu->r[rm] * cpu->r[rs];
    if (op & (1u << 21)) result += ((uint64_t)cpu->r[hi] << 32) | cpu->r[lo];
    cpu->r[lo] = (uint32_t)result;
    cpu->r[hi] = (uint32_t)(result >> 32);
    if (op & (1u << 20)) {
        cpu->cpsr = (cpu->cpsr & ~(ARM7_FLAG_N | ARM7_FLAG_Z)) |
                    ((result >> 32) & ARM7_FLAG_N) | (result ? 0 : ARM7_FLAG_Z);
    }
}

static void swap(arm7_t *cpu, uint32_t op) {
    uint32_t address = cpu->r[(op >> 16) & 15], rd = (op >> 12) & 15, source = cpu->r[op & 15];
    if (op & (1u << 22)) {
        uint8_t old = cpu->read8(cpu->bus, address);
        cpu->write8(cpu->bus, address, (uint8_t)source);
        cpu->r[rd] = old;
    } else {
        uint32_t old = load_word(cpu, address);
        cpu->write32(cpu->bus, address & ~3u, source);
        cpu->r[rd] = old;
    }
}

static void single_transfer(arm7_t *cpu, uint32_t op) {
    bool pre = (op >> 24) & 1, up = (op >> 23) & 1, byte = (op >> 22) & 1;
    bool writeback = (op >> 21) & 1, load = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, offset;
    if (op & (1u << 25)) {
        bool carry;
        offset = shifted_register(cpu, op & ~0x10u, &carry);
    } else {
        offset = op & 0xfff;
    }
    uint32_t base = reg(cpu, rn);
    uint32_t moved = up ? base + offset : base - offset;
    uint32_t address = pre ? moved : base;
    if (load) {
        uint32_t value = byte ? cpu->read8(cpu->bus, address) : load_word(cpu, address);
        if (!pre || writeback) cpu->r[rn] = moved;
        if (rd == 15) write_pc(cpu, value);
        else cpu->r[rd] = value;
    } else {
        uint32_t value = rd == 15 ? cpu->r[15] + 8 : cpu->r[rd];
        if (byte) cpu->write8(cpu->bus, address, (uint8_t)value);
        else cpu->write32(cpu->bus, address & ~3u, value);
        if (!pre || writeback) cpu->r[rn] = moved;
    }
}

/* ARMv4 halfword and signed transfers; the firmware does not use them. */
static void halfword_transfer(arm7_t *cpu, uint32_t op) {
    bool pre = (op >> 24) & 1, up = (op >> 23) & 1, immediate = (op >> 22) & 1;
    bool writeback = (op >> 21) & 1, load = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, kind = (op >> 5) & 3;
    uint32_t offset = immediate ? (((op >> 4) & 0xf0) | (op & 15)) : cpu->r[op & 15];
    uint32_t base = reg(cpu, rn);
    uint32_t moved = up ? base + offset : base - offset;
    uint32_t address = pre ? moved : base;
    if (load) {
        uint32_t value;
        if (kind == 2) value = (uint32_t)(int32_t)(int8_t)cpu->read8(cpu->bus, address);
        else {
            uint32_t word = cpu->read32(cpu->bus, address & ~3u);
            uint16_t half = (uint16_t)(word >> ((address & 2u) * 8u));
            value = kind == 3 ? (uint32_t)(int32_t)(int16_t)half : half;
        }
        if (!pre || writeback) cpu->r[rn] = moved;
        if (rd == 15) write_pc(cpu, value);
        else cpu->r[rd] = value;
    } else {
        uint32_t value = rd == 15 ? cpu->r[15] + 8 : cpu->r[rd];
        uint32_t word = cpu->read32(cpu->bus, address & ~3u);
        uint32_t shift = (address & 2u) * 8u;
        word = (word & ~(0xffffu << shift)) | ((value & 0xffffu) << shift);
        cpu->write32(cpu->bus, address & ~3u, word);
        if (!pre || writeback) cpu->r[rn] = moved;
    }
}

static void block_transfer(arm7_t *cpu, uint32_t op) {
    bool pre = (op >> 24) & 1, up = (op >> 23) & 1, user = (op >> 22) & 1;
    bool writeback = (op >> 21) & 1, load = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, list = op & 0xffff;
    uint32_t count = 0;
    for (uint32_t i = 0; i < 16; ++i) count += (list >> i) & 1;
    uint32_t base = cpu->r[rn];
    uint32_t address = up ? base + (pre ? 4 : 0) : base - 4 * count + (pre ? 0 : 4);
    uint32_t final = up ? base + 4 * count : base - 4 * count;
    bool user_bank = user && !(load && (list & 0x8000));
    uint32_t mode = cpu->cpsr & 0x1f;
    if (user_bank) switch_mode(cpu, ARM7_MODE_USR);
    if (load) {
        if (writeback) cpu->r[rn] = final;
        for (uint32_t i = 0; i < 16; ++i) {
            if (!(list & (1u << i))) continue;
            uint32_t value = cpu->read32(cpu->bus, address & ~3u);
            address += 4;
            if (i == 15) write_pc(cpu, value);
            else cpu->r[i] = value;
        }
    } else {
        bool first = true;
        for (uint32_t i = 0; i < 16; ++i) {
            if (!(list & (1u << i))) continue;
            uint32_t value = i == 15 ? cpu->r[15] + 8 : (i == rn && first ? base : cpu->r[i]);
            cpu->write32(cpu->bus, address & ~3u, value);
            address += 4;
            first = false;
        }
        if (writeback) cpu->r[rn] = final;
    }
    if (user_bank) switch_mode(cpu, mode);
    if (user && load && (list & 0x8000)) {
        uint32_t *saved = spsr(cpu);
        if (saved) set_cpsr(cpu, *saved);
    }
}

static void execute(arm7_t *cpu, uint32_t op) {
    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x0ffffff0u) == 0x012fff10u) { /* BX, ARMv4T: ARM state only. */
            write_pc(cpu, cpu->r[op & 15]);
        } else if ((op & 0x0fc000f0u) == 0x00000090u) {
            multiply(cpu, op);
        } else if ((op & 0x0f8000f0u) == 0x00800090u) {
            multiply_long(cpu, op);
        } else if ((op & 0x0fb00ff0u) == 0x01000090u) {
            swap(cpu, op);
        } else if ((op & 0x0e000090u) == 0x00000090u) {
            halfword_transfer(cpu, op);
        } else if ((op & 0x0fbf0fffu) == 0x010f0000u || (op & 0x0fb0fff0u) == 0x0120f000u) {
            psr_transfer(cpu, op);
        } else {
            data_processing(cpu, op);
        }
        break;
    case 1:
        if ((op & 0x0fb0f000u) == 0x0320f000u) psr_transfer(cpu, op);
        else data_processing(cpu, op);
        break;
    case 2:
        single_transfer(cpu, op);
        break;
    case 3:
        if (op & 0x10) exception(cpu, ARM7_MODE_UND, 0x04, cpu->r[15], false);
        else single_transfer(cpu, op);
        break;
    case 4:
        block_transfer(cpu, op);
        break;
    case 5: {
        int32_t offset = (int32_t)(op << 8) >> 6;
        if (op & (1u << 24)) cpu->r[14] = cpu->r[15];
        write_pc(cpu, cpu->r[15] + 4 + (uint32_t)offset);
        break;
    }
    case 6:
        exception(cpu, ARM7_MODE_UND, 0x04, cpu->r[15], false);
        break;
    default:
        if (op & (1u << 24)) exception(cpu, ARM7_MODE_SVC, 0x08, cpu->r[15], false);
        else exception(cpu, ARM7_MODE_UND, 0x04, cpu->r[15], false);
        break;
    }
}

void arm7_run(arm7_t *cpu, uint32_t count) {
    while (count-- && !cpu->halted) {
        if (cpu->fiq_line && !(cpu->cpsr & ARM7_FLAG_F))
            exception(cpu, ARM7_MODE_FIQ, 0x1c, cpu->r[15] + 4, true);
        uint32_t pc = cpu->r[15];
        uint32_t op = cpu->read32(cpu->bus, pc);
        cpu->r[15] = pc + 4;
        if (condition(op >> 28, cpu->cpsr)) execute(cpu, op);
        ++cpu->instructions;
    }
}
