/* A software AICA. The envelope rate tables, the LFO tables, the ADPCM
 * decoder and the DSP step follow MAME's src/devices/sound/aica.cpp and
 * aicadsp.cpp:
 *
 *   license:BSD-3-Clause
 *   copyright-holders:ElSemi, Deunan Knute, R. Belmont
 *
 * The filter, the envelope's attack shape and the mixer's gain are this
 * file's own; see docs/design/host-backend.md. */
#include "aica.h"

#include <math.h>
#include <string.h>

enum { AICA_EG_ATTACK, AICA_EG_DECAY1, AICA_EG_DECAY2, AICA_EG_RELEASE };
enum {
    F_CONTROL, F_SA_LOW, F_LSA, F_LEA, F_ENV_AD, F_ENV_DR, F_PITCH, F_LFO,
    F_DSP_SEND, F_DIRECT, F_MIX, F_FLV0, F_FLV1, F_FLV2, F_FLV3, F_FLV4, F_FEG_AD, F_FEG_DR
};
#define CH(chip, ch) (&(chip)->reg[(ch) * 32u])
#define REG(offset) ((offset) >> 2)

/* Common register offsets. */
#define R_COMMON 0x2800u
#define R_RING 0x2804u
#define R_MONITOR_SELECT 0x280cu
#define R_MONITOR 0x2810u
#define R_MONITOR_CA 0x2814u
#define R_TIMA 0x2890u
#define R_SCIEB 0x289cu
#define R_SCIPD 0x28a0u
#define R_SCIRE 0x28a4u
#define R_SCILV0 0x28a8u
#define R_MCIEB 0x28b4u
#define R_MCIPD 0x28b8u
#define R_MCIRE 0x28bcu
#define R_ARMRST 0x2c00u
#define R_INTREQ 0x2d00u
#define R_INTCLEAR 0x2d04u
#define R_EFS 0x2000u
#define R_COEF 0x3000u
#define R_MADRS 0x3200u
#define R_MPRO 0x3400u
#define R_TEMP 0x4000u
#define R_MEMS 0x4400u
#define R_MIXS 0x4500u
#define R_EFREG 0x4580u
#define R_EXTS 0x45c0u

/* Envelope times in ms, from MAME (Yamaha's SCSP datasheet). */
static const double ar_times[64] = {
    100000, 100000, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0, 1700.0, 1500.0,
    1200.0, 1000.0, 860.0, 760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0, 190.0, 150.0, 130.0, 110.0, 95.0,
    76.0, 63.0, 55.0, 47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0, 12.0, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4,
    2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0.0, 0.0};
static const double dr_times[64] = {
    100000, 100000, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0, 29600.0, 25300.0, 22200.0, 17700.0,
    14800.0, 12700.0, 11100.0, 8900.0, 7400.0, 6300.0, 5500.0, 4400.0, 3700.0, 3200.0, 2800.0, 2200.0, 1800.0, 1600.0, 1400.0, 1100.0,
    920.0, 790.0, 690.0, 550.0, 460.0, 390.0, 340.0, 270.0, 230.0, 200.0, 170.0, 140.0, 110.0, 98.0, 85.0, 68.0, 57.0, 49.0, 43.0, 34.0,
    28.0, 25.0, 22.0, 18.0, 14.0, 12.0, 11.0, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1};
static const float lfo_freq[32] = {
    0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f, 0.55f, 0.68f, 0.78f, 0.92f, 1.10f, 1.39f, 1.60f, 1.87f, 2.27f,
    2.87f, 3.31f, 3.92f, 4.79f, 6.15f, 7.18f, 8.60f, 10.8f, 14.4f, 17.2f, 21.5f, 28.7f, 43.1f, 57.4f, 86.1f, 172.3f};
static const float alfo_scale_db[8] = {0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f};
static const float plfo_scale_cents[8] = {0.0f, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f};

static struct {
    bool ready;
    float attack_factor[64];  /* Per-sample (att + 1) multiplier while attacking. */
    float decay_step[64];     /* Per-sample attenuation increase. */
    float feg_step[32];       /* Per-sample FLV change for a filter rate. */
    float eg_gain[1024];
    float tl_gain[256];
    float level_gain[16];     /* DISDL, IMXL, EFSDL and MVOL: 3 dB steps, 0 is off. */
    float pan_gain[16];       /* 3 dB steps; 15 is off. */
    float filter_f[8192];     /* Twice-oversampled SVF coefficient for an FLV. */
    float filter_damping[32];
    int16_t alfo_wave[4][256];
    int16_t plfo_wave[4][256];
} tables;

static void tables_init(void) {
    if (tables.ready) return;
    for (int i = 0; i < 64; ++i) {
        double t = ar_times[i];
        if (i < 2) tables.attack_factor[i] = 1.0f;
        else if (t == 0.0) tables.attack_factor[i] = 0.0f;
        else tables.attack_factor[i] = (float)exp(-log(1024.0) / (t * AICA_RATE / 1000.0));
        tables.decay_step[i] = i < 2 ? 0.0f : (float)(1024.0 / (dr_times[i] * AICA_RATE / 1000.0));
    }
    for (int r = 0; r < 32; ++r)
        tables.feg_step[r] = r ? (float)(1024.0 / (dr_times[2 * r] * AICA_RATE / 1000.0)) : 0.0f;
    for (int i = 0; i < 1024; ++i) tables.eg_gain[i] = (float)pow(10.0, -(i * 3.0 / 32.0) / 20.0);
    tables.eg_gain[1023] = 0.0f;
    for (int i = 0; i < 256; ++i) tables.tl_gain[i] = (float)pow(10.0, -(i * 0.375) / 20.0);
    tables.level_gain[0] = 0.0f;
    for (int i = 1; i < 16; ++i) tables.level_gain[i] = (float)pow(10.0, (-42.0 + 3.0 * (i - 1)) / 20.0);
    for (int i = 0; i < 15; ++i) tables.pan_gain[i] = (float)pow(10.0, -(3.0 * i) / 20.0);
    tables.pan_gain[15] = 0.0f;
    /* AICAforge encodes a cutoff as c = 2 sin(pi f / 44100), stored as
     * (exponent << 9) | (mantissa - 512) with c = mantissa * 2^(exponent - 25). */
    for (int level = 0; level < 8192; ++level) {
        double c = (512.0 + (level & 511)) * pow(2.0, (level >> 9) - 25.0);
        if (c > 1.999) c = 1.999;
        double cutoff = asin(c / 2.0) * AICA_RATE / acos(-1.0);
        tables.filter_f[level] = (float)(2.0 * sin(acos(-1.0) * cutoff / (2.0 * AICA_RATE)));
    }
    for (int q = 0; q < 32; ++q) tables.filter_damping[q] = (float)pow(10.0, -(0.75 * q - 3.0) / 20.0);
    uint32_t seed = 0x2545f491u;
    for (int i = 0; i < 256; ++i) {
        tables.alfo_wave[0][i] = (int16_t)(255 - i);
        tables.plfo_wave[0][i] = (int16_t)(i < 128 ? i : i - 256);
        tables.alfo_wave[1][i] = i < 128 ? 255 : 0;
        tables.plfo_wave[1][i] = i < 128 ? 127 : -128;
        tables.alfo_wave[2][i] = (int16_t)(i < 128 ? 255 - i * 2 : i * 2 - 256);
        tables.plfo_wave[2][i] = (int16_t)(i < 64 ? i * 2 : i < 128 ? 255 - i * 2 : i < 192 ? 256 - i * 2 : i * 2 - 511);
        seed = seed * 1103515245u + 12345u;
        int noise = (seed >> 16) & 0xff;
        tables.alfo_wave[3][i] = (int16_t)noise;
        tables.plfo_wave[3][i] = (int16_t)(128 - noise);
    }
    tables.ready = true;
}

/* The ARM7's view of the bus: RAM mirrored below 8 MiB, registers above. */
static uint32_t bus_read32(void *bus, uint32_t address) {
    aica_t *chip = bus;
    if (address < 0x800000u) {
        const uint8_t *p = chip->ram + (address & (AICA_RAM_BYTES - 4u));
        return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    if (address - 0x800000u < AICA_REG_BYTES) return aica_reg_read(chip, address - 0x800000u);
    return 0;
}
static uint8_t bus_read8(void *bus, uint32_t address) {
    aica_t *chip = bus;
    if (address < 0x800000u) return chip->ram[address & (AICA_RAM_BYTES - 1u)];
    if (address - 0x800000u < AICA_REG_BYTES)
        return (uint8_t)(aica_reg_read(chip, (address - 0x800000u) & ~3u) >> ((address & 3u) * 8u));
    return 0;
}
static void bus_write32(void *bus, uint32_t address, uint32_t value) {
    aica_t *chip = bus;
    if (address < 0x800000u) {
        uint8_t *p = chip->ram + (address & (AICA_RAM_BYTES - 4u));
        p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
        p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
    } else if (address - 0x800000u < AICA_REG_BYTES) {
        aica_reg_write(chip, address - 0x800000u, value);
    }
}
static void bus_write8(void *bus, uint32_t address, uint8_t value) {
    aica_t *chip = bus;
    if (address < 0x800000u) {
        chip->ram[address & (AICA_RAM_BYTES - 1u)] = value;
    } else if (address - 0x800000u < AICA_REG_BYTES) {
        uint32_t offset = (address - 0x800000u) & ~3u, shift = (address & 3u) * 8u;
        uint32_t word = chip->reg[REG(offset)];
        aica_reg_write(chip, offset, (word & ~(0xffu << shift)) | ((uint32_t)value << shift));
    }
}

static void update_fiq(aica_t *chip) {
    chip->arm.fiq_line = (chip->scipd & chip->reg[REG(R_SCIEB)] & 0x7ffu) != 0;
}

void aica_init(aica_t *chip) {
    tables_init();
    memset(chip, 0, sizeof(*chip));
    chip->arm.bus = chip;
    chip->arm.read32 = bus_read32;
    chip->arm.read8 = bus_read8;
    chip->arm.write32 = bus_write32;
    chip->arm.write8 = bus_write8;
    arm7_reset(&chip->arm);
    chip->arm_reset = true;
    chip->reg[REG(R_ARMRST)] = 1;
    for (int t = 0; t < 3; ++t) { chip->timer_count[t] = 0xffffu; chip->timer_expired[t] = true; }
    chip->cycles_per_sample = 128;
    chip->lfo_noise = 1;
    chip->dsp.program_dirty = true;
}

void aica_arm_reset(aica_t *chip, bool held) {
    if (held == chip->arm_reset) return;
    chip->arm_reset = held;
    chip->reg[REG(R_ARMRST)] = held ? 1 : 0;
    if (!held) arm7_reset(&chip->arm);
    update_fiq(chip);
}

static int8_t signed4(uint32_t value) { return (int8_t)(value > 7 ? (int)value - 16 : (int)value); }

static int eg_rate(const uint16_t *r, uint32_t rate_register) {
    if (!rate_register) return 0;
    int base = 0, krs = (r[F_ENV_DR] >> 10) & 15;
    if (krs != 15) base = signed4((r[F_PITCH] >> 11) & 15) + 2 * krs + ((r[F_PITCH] >> 9) & 1);
    int rate = base + 2 * (int)rate_register;
    return rate < 0 ? 0 : rate > 63 ? 63 : rate;
}

static void key_on(aica_t *chip, uint32_t ch) {
    aica_voice_t *v = &chip->voice[ch];
    const uint16_t *r = CH(chip, ch);
    memset(v, 0, sizeof(*v));
    v->active = true;
    v->eg_state = AICA_EG_ATTACK;
    v->att = 1023.0f;
    v->adpcm_quant = 0x7f;
    v->noise = 0x1ffffu;
    v->feg_state = AICA_EG_ATTACK;
    v->feg_level = (float)(r[F_FLV0] & 0x1fff);
}

static void key_off(aica_t *chip, uint32_t ch) {
    aica_voice_t *v = &chip->voice[ch];
    if (!v->active) return;
    v->eg_state = AICA_EG_RELEASE;
    v->feg_state = AICA_EG_RELEASE;
}

/* KYONEX: start channels keyed on that are silent or releasing, release
 * those keyed off. A channel already sounding is not restarted. */
static void key_execute(aica_t *chip) {
    for (uint32_t ch = 0; ch < AICA_VOICES; ++ch) {
        bool keyed = CH(chip, ch)[F_CONTROL] & 0x4000u;
        aica_voice_t *v = &chip->voice[ch];
        if (keyed && (!v->active || v->eg_state == AICA_EG_RELEASE)) key_on(chip, ch);
        else if (!keyed) key_off(chip, ch);
    }
}

void aica_key_off_all(aica_t *chip) {
    for (uint32_t ch = 0; ch < AICA_VOICES; ++ch) {
        CH(chip, ch)[F_CONTROL] &= ~0x4000u;
        chip->voice[ch].active = false;
    }
}

static uint32_t interrupt_level(const aica_t *chip) {
    static const uint8_t order[11] = {6, 7, 8, 0, 1, 2, 3, 4, 5, 9, 10};
    uint32_t pending = chip->scipd & chip->reg[REG(R_SCIEB)];
    for (int i = 0; i < 11; ++i) {
        uint32_t bit = order[i];
        if (!(pending & (1u << bit))) continue;
        uint32_t source = bit > 7 ? 7 : bit, level = 0;
        for (uint32_t k = 0; k < 3; ++k)
            if (chip->reg[REG(R_SCILV0) + k] & (1u << source)) level |= 1u << k;
        return level;
    }
    return 0;
}

uint32_t aica_reg_read(aica_t *chip, uint32_t offset) {
    offset &= (AICA_REG_BYTES - 1u) & ~3u;
    if (offset < 0x2000u) return chip->reg[REG(offset)];
    switch (offset) {
    case R_MONITOR: {
        uint32_t slot = (chip->reg[REG(R_MONITOR_SELECT)] >> 8) & 63;
        aica_voice_t *v = &chip->voice[slot];
        uint32_t value = (v->lpend ? 0x8000u : 0) | ((uint32_t)(v->eg_state & 3) << 13);
        value |= v->active ? ((uint32_t)v->att << 3) & 0x1ff8u : 0x1ff8u;
        v->lpend = false;
        return value;
    }
    case R_MONITOR_CA:
        return chip->voice[(chip->reg[REG(R_MONITOR_SELECT)] >> 8) & 63].position >> 12 & 0xffffu;
    case R_TIMA: case R_TIMA + 4: case R_TIMA + 8: {
        uint32_t t = (offset - R_TIMA) / 4;
        return (chip->reg[REG(offset)] & 0x700u) | ((chip->timer_count[t] >> 8) & 0xffu);
    }
    case R_SCIPD: return chip->scipd;
    case R_MCIPD: return chip->mcipd;
    case R_INTREQ: return interrupt_level(chip);
    default: break;
    }
    if (offset >= R_TEMP && offset < R_MEMS) {
        int32_t value = chip->dsp.temp[((offset - R_TEMP) >> 3) & 127];
        return offset & 4 ? value & 0xffff : (value >> 16) & 0xff;
    }
    if (offset >= R_MEMS && offset < R_MIXS) {
        int32_t value = chip->dsp.mems[((offset - R_MEMS) >> 3) & 31];
        return offset & 4 ? value & 0xffff : (value >> 16) & 0xff;
    }
    if (offset >= R_MIXS && offset < R_EFREG) {
        int32_t value = chip->dsp.mixs[((offset - R_MIXS) >> 3) & 15];
        return offset & 4 ? value & 0xffff : (value >> 16) & 0xff;
    }
    if (offset >= R_EFREG && offset < R_EXTS) return (uint16_t)chip->dsp.efreg[(offset - R_EFREG) >> 2];
    if (offset >= R_EXTS && offset < R_EXTS + 8) return 0;
    return chip->reg[REG(offset)];
}

void aica_reg_write(aica_t *chip, uint32_t offset, uint32_t value) {
    offset &= (AICA_REG_BYTES - 1u) & ~3u;
    uint16_t word = (uint16_t)value;
    if (offset < 0x2000u) {
        if ((offset & 0x7fu) >= 18u * 4u) return;
        if ((offset & 0x7fu) == 0) {
            chip->reg[REG(offset)] = word & 0x7fffu;
            if (word & 0x8000u) key_execute(chip);
        } else {
            chip->reg[REG(offset)] = word;
        }
        return;
    }
    switch (offset) {
    case R_TIMA: case R_TIMA + 4: case R_TIMA + 8: {
        uint32_t t = (offset - R_TIMA) / 4;
        chip->reg[REG(offset)] = word;
        chip->timer_count[t] = (uint32_t)(word & 0xffu) << 8;
        chip->timer_expired[t] = false;
        return;
    }
    case R_SCIPD:
        if (word & 0x20u) chip->scipd |= 0x20u;
        update_fiq(chip);
        return;
    case R_SCIRE:
        chip->scipd &= ~(uint32_t)word;
        /* As on the SCSP: resetting an expired timer's bit raises it again. */
        for (int t = 0; t < 3; ++t)
            if (chip->timer_expired[t]) chip->scipd |= 0x40u << t;
        update_fiq(chip);
        return;
    case R_SCIEB:
        chip->reg[REG(offset)] = word;
        update_fiq(chip);
        return;
    case R_MCIPD:
        if (word & 0x20u) chip->mcipd |= 0x20u;
        return;
    case R_MCIRE:
        chip->mcipd &= ~(uint32_t)word;
        return;
    case R_ARMRST:
        aica_arm_reset(chip, word & 1u);
        return;
    case R_INTCLEAR:
    case R_INTREQ:
        return;
    default: break;
    }
    if (offset >= R_MPRO && offset < R_MPRO + 128u * 16u) chip->dsp.program_dirty = true;
    if (offset >= R_TEMP && offset < R_MEMS) {
        int32_t *t = &chip->dsp.temp[((offset - R_TEMP) >> 3) & 127];
        *t = offset & 4 ? (*t & ~0xffff) | word : (*t & 0xffff) | (int32_t)((uint32_t)word << 16);
        return;
    }
    if (offset >= R_MEMS && offset < R_MIXS) {
        int32_t *m = &chip->dsp.mems[((offset - R_MEMS) >> 3) & 31];
        *m = offset & 4 ? (*m & ~0xffff) | word : (*m & 0xffff) | (int32_t)((uint32_t)word << 16);
        return;
    }
    if (offset >= R_MIXS && offset < R_EFREG) return; /* Read-only. */
    if (offset >= R_EFREG && offset < R_EXTS) {
        chip->dsp.efreg[(offset - R_EFREG) >> 2] = (int16_t)word;
        return;
    }
    chip->reg[REG(offset)] = word;
}

void aica_run_cpu(aica_t *chip, uint32_t instructions) {
    if (!chip->arm_reset) arm7_run(&chip->arm, instructions);
}

bool aica_main_irq(const aica_t *chip) {
    return (chip->mcipd & chip->reg[REG(R_MCIEB)]) != 0;
}

static void timers_tick(aica_t *chip) {
    for (int t = 0; t < 3; ++t) {
        if (chip->timer_expired[t]) continue;
        uint32_t prescale = (chip->reg[REG(R_TIMA) + t] >> 8) & 7;
        chip->timer_count[t] += 256u >> prescale;
        if (chip->timer_count[t] > 0xff00u) {
            chip->timer_count[t] = 0xffffu;
            chip->timer_expired[t] = true;
            chip->scipd |= 0x40u << t;
            chip->mcipd |= 0x40u << t;
        }
    }
    update_fiq(chip);
}

/* ---- DSP, after MAME's aicadsp.cpp ---- */

static uint16_t dsp_pack(int32_t val) {
    int sign = (val >> 23) & 1;
    uint32_t temp = (uint32_t)(val ^ (val << 1)) & 0xffffffu;
    int exponent = 0;
    for (int k = 0; k < 12; ++k) {
        if (temp & 0x800000u) break;
        temp <<= 1;
        exponent += 1;
    }
    if (exponent < 12) val = (int32_t)(((uint32_t)val << exponent) & 0x3fffffu);
    else val = (int32_t)((uint32_t)val << 11);
    val >>= 11;
    val &= 0x7ff;
    val |= sign << 15;
    val |= exponent << 11;
    return (uint16_t)val;
}

static int32_t dsp_unpack(uint16_t val) {
    int sign = (val >> 15) & 1, exponent = (val >> 11) & 0xf, mantissa = val & 0x7ff;
    uint32_t uval = (uint32_t)mantissa << 11;
    if (exponent > 11) {
        exponent = 11;
        uval |= (uint32_t)sign << 22;
    } else {
        uval |= (uint32_t)(sign ^ 1) << 22;
    }
    uval |= (uint32_t)sign << 23;
    uval <<= 8;
    int32_t sval = (int32_t)uval >> 8;
    return sval >> exponent;
}

static int32_t sign24(int32_t value) { return (int32_t)((uint32_t)value << 8) >> 8; }
static int32_t clamp24(int32_t value) {
    return value < -0x800000 ? -0x800000 : value > 0x7fffff ? 0x7fffff : value;
}

static void dsp_step(aica_t *chip) {
    aica_dsp_t *d = &chip->dsp;
    const uint16_t *mpro = &chip->reg[REG(R_MPRO)];
    if (d->program_dirty) {
        int last = 127;
        for (; last >= 0; --last) {
            const uint16_t *s = mpro + last * 4;
            if (s[0] || s[1] || s[2] || s[3]) break;
        }
        d->last_step = last + 1;
        d->program_dirty = false;
    }
    memset(d->efreg, 0, sizeof(d->efreg));
    if (d->last_step) {
        int32_t acc = 0, memval = 0, frc_reg = 0, y_reg = 0;
        uint32_t adrs_reg = 0;
        uint32_t ring = chip->reg[REG(R_RING)];
        uint32_t rbl_words = (8u * 1024u) << ((ring >> 13) & 3), rbp = ring & 0xfffu;
        for (int step = 0; step < d->last_step; ++step) {
            const uint16_t *ip = mpro + step * 4;
            uint32_t tra = (ip[0] >> 9) & 0x7f, twt = (ip[0] >> 8) & 1, twa = (ip[0] >> 1) & 0x7f;
            uint32_t xsel = (ip[1] >> 15) & 1, ysel = (ip[1] >> 13) & 3, ira = (ip[1] >> 7) & 0x3f;
            uint32_t iwt = (ip[1] >> 6) & 1, iwa = (ip[1] >> 1) & 0x1f;
            uint32_t table = (ip[2] >> 15) & 1, mwt = (ip[2] >> 14) & 1, mrd = (ip[2] >> 13) & 1;
            uint32_t ewt = (ip[2] >> 12) & 1, ewa = (ip[2] >> 8) & 0xf, adrl = (ip[2] >> 7) & 1;
            uint32_t frcl = (ip[2] >> 6) & 1, shift = (ip[2] >> 4) & 3, yrl = (ip[2] >> 3) & 1;
            uint32_t negb = (ip[2] >> 2) & 1, zero = (ip[2] >> 1) & 1, bsel = ip[2] & 1;
            uint32_t nofl = (ip[3] >> 15) & 1, masa = (ip[3] >> 9) & 0x1f;
            uint32_t adreb = (ip[3] >> 8) & 1, nxadr = (ip[3] >> 7) & 1;
            int32_t inputs = 0;
            if (ira <= 0x1f) inputs = d->mems[ira];
            else if (ira <= 0x2f) inputs = d->mixs[ira - 0x20] << 4;
            inputs = sign24(inputs);
            if (iwt) {
                d->mems[iwa] = memval;
                if (ira == iwa) inputs = memval;
            }
            int32_t b = 0;
            if (!zero) {
                b = bsel ? acc : sign24(d->temp[(tra + d->dec) & 0x7f]);
                if (negb) b = -b;
            }
            int32_t x = xsel ? inputs : sign24(d->temp[(tra + d->dec) & 0x7f]);
            int32_t y = 0;
            if (ysel == 0) y = frc_reg;
            else if (ysel == 1) y = (int16_t)chip->reg[REG(R_COEF) + step] >> 3;
            else if (ysel == 2) y = (y_reg >> 11) & 0x1fff;
            else y = (y_reg >> 4) & 0x0fff;
            if (yrl) y_reg = inputs;
            int32_t shifted;
            if (shift == 0) shifted = clamp24(acc);
            else if (shift == 1) shifted = clamp24(acc * 2);
            else if (shift == 2) shifted = sign24(acc * 2);
            else shifted = sign24(acc);
            y = (int32_t)((uint32_t)y << 19) >> 19;
            acc = (int32_t)(((int64_t)x * y) >> 12) + b;
            if (twt) d->temp[(twa + d->dec) & 0x7f] = shifted;
            if (frcl) frc_reg = shift == 3 ? shifted & 0x0fff : (shifted >> 11) & 0x1fff;
            if (mrd || mwt) {
                uint32_t address = chip->reg[REG(R_MADRS) + masa];
                if (!table) address += d->dec;
                if (adreb) address += adrs_reg & 0x0fff;
                if (nxadr) address++;
                address &= table ? 0xffffu : rbl_words - 1u;
                address += rbp << 10;
                uint8_t *p = chip->ram + ((address * 2u) & (AICA_RAM_BYTES - 2u));
                if (mrd && (step & 1)) {
                    uint16_t word16 = (uint16_t)(p[0] | p[1] << 8);
                    memval = nofl ? (int32_t)((uint32_t)word16 << 8) : dsp_unpack(word16);
                    if (nofl) memval = sign24(memval);
                }
                if (mwt && (step & 1)) {
                    uint16_t word16 = nofl ? (uint16_t)(shifted >> 8) : dsp_pack(shifted);
                    p[0] = (uint8_t)word16;
                    p[1] = (uint8_t)(word16 >> 8);
                }
            }
            if (adrl) adrs_reg = shift == 3 ? (uint32_t)(shifted >> 12) & 0xfff : (uint32_t)(inputs >> 16);
            if (ewt) d->efreg[ewa] = (int16_t)(d->efreg[ewa] + (shifted >> 8));
        }
        --d->dec;
    }
    memset(d->mixs, 0, sizeof(d->mixs));
}

/* ---- Voices ---- */

static const int32_t adpcm_quant_table[8] = {230, 230, 230, 230, 307, 409, 512, 614};
static const int32_t adpcm_multiplier[8] = {1, 3, 5, 7, 9, 11, 13, 15};

static void adpcm_decode_next(aica_t *chip, aica_voice_t *v, uint32_t sa, uint32_t lsa) {
    if (v->adpcm_decoded == lsa && !v->loop_saved) {
        v->loop_signal = v->adpcm_current;
        v->loop_quant = v->adpcm_quant;
        v->loop_saved = true;
    }
    uint32_t n = v->adpcm_decoded;
    uint8_t byte = chip->ram[(sa + n / 2) & (AICA_RAM_BYTES - 1u)];
    uint32_t delta = (byte >> ((n & 1) * 4)) & 15;
    int32_t x = (v->adpcm_quant * adpcm_multiplier[delta & 7]) / 8;
    if (x > 0x7fff) x = 0x7fff;
    if (delta & 8) x = -x;
    x += v->adpcm_current;
    x = x < -32768 ? -32768 : x > 32767 ? 32767 : x;
    v->adpcm_previous = v->adpcm_current;
    v->adpcm_current = x;
    int32_t quant = (v->adpcm_quant * adpcm_quant_table[delta & 7]) >> 8;
    v->adpcm_quant = quant < 0x7f ? 0x7f : quant > 0x6000 ? 0x6000 : quant;
    ++v->adpcm_decoded;
}

static int32_t read16(const aica_t *chip, uint32_t address) {
    const uint8_t *p = chip->ram + (address & (AICA_RAM_BYTES - 2u));
    return (int16_t)(p[0] | p[1] << 8);
}

/* One voice's sample before its levels, or false once it has stopped. */
static bool voice_sample(aica_t *chip, uint32_t ch, float *out) {
    aica_voice_t *v = &chip->voice[ch];
    const uint16_t *r = CH(chip, ch);
    uint32_t control = r[F_CONTROL];
    uint32_t pcms = (control >> 7) & 3;
    bool loop = control & 0x200u, noise = control & 0x400u;
    uint32_t sa = (((control & 0x7fu) << 16) | r[F_SA_LOW]) & (AICA_RAM_BYTES - 1u);
    uint32_t lsa = r[F_LSA], lea = r[F_LEA];
    uint32_t lfo = r[F_LFO];
    uint32_t lfo_step = (uint32_t)(lfo_freq[(lfo >> 10) & 31] * 256.0f * 256.0f / AICA_RATE);
    bool lfo_reset = lfo & 0x8000u;

    int oct = signed4((r[F_PITCH] >> 11) & 15) + 2;
    uint32_t step = 1024u + (r[F_PITCH] & 0x3ffu);
    step = oct >= 0 ? step << oct : step >> -oct;
    uint32_t plfos = (lfo >> 5) & 7;
    if (plfos) {
        int wave = tables.plfo_wave[(lfo >> 8) & 3][(v->plfo_phase >> 8) & 255];
        float cents = plfo_scale_cents[plfos] * wave / 128.0f;
        step = (uint32_t)(step * exp2f(cents / 1200.0f));
    }
    v->plfo_phase = lfo_reset ? 0 : v->plfo_phase + lfo_step;

    uint32_t index = v->position >> 12, frac = v->position & 4095u;
    int32_t a, b;
    if (noise) {
        uint32_t bit = ((v->noise >> 0) ^ (v->noise >> 3)) & 1;
        v->noise = (v->noise >> 1) | (bit << 16);
        a = b = (int32_t)(v->noise & 0xffffu) - 32768;
    } else if (pcms == 0) {
        uint32_t next = index + 1 < lea ? index + 1 : loop ? lsa : index;
        a = read16(chip, sa + index * 2);
        b = read16(chip, sa + next * 2);
    } else if (pcms == 1) {
        uint32_t next = index + 1 < lea ? index + 1 : loop ? lsa : index;
        a = (int8_t)chip->ram[(sa + index) & (AICA_RAM_BYTES - 1u)] * 256;
        b = (int8_t)chip->ram[(sa + next) & (AICA_RAM_BYTES - 1u)] * 256;
    } else {
        while (v->adpcm_decoded < index + 2) adpcm_decode_next(chip, v, sa, lsa);
        a = v->adpcm_previous;
        b = index + 1 < lea ? v->adpcm_current : a;
    }
    float sample = (float)a + (float)(b - a) * (float)frac * (1.0f / 4096.0f);

    v->position += step;
    uint32_t moved = v->position >> 12;
    if ((r[F_ENV_DR] & 0x4000u) && v->eg_state == AICA_EG_ATTACK && moved >= lsa)
        v->eg_state = AICA_EG_DECAY1;
    if (moved >= lea && !noise) {
        v->lpend = true;
        if (!loop || lea <= lsa) {
            v->active = false;
        } else {
            uint32_t length = (lea - lsa) << 12;
            while ((v->position >> 12) >= lea) v->position -= length;
            if (pcms >= 2) {
                /* Resume decoding at LSA; PCMS 2 also restores the predictor. */
                v->adpcm_decoded = lsa;
                if (pcms == 2 && v->loop_saved) {
                    v->adpcm_current = v->loop_signal;
                    v->adpcm_quant = v->loop_quant;
                }
            }
        }
    }

    /* Filter: a state-variable low-pass run twice per sample. */
    if (!(r[F_MIX] & 0x20u)) {
        static const uint8_t feg_target[4] = {F_FLV1, F_FLV2, F_FLV3, F_FLV4};
        uint32_t rate;
        switch (v->feg_state) {
        case AICA_EG_ATTACK: rate = (r[F_FEG_AD] >> 8) & 31; break;
        case AICA_EG_DECAY1: rate = r[F_FEG_AD] & 31; break;
        case AICA_EG_DECAY2: rate = (r[F_FEG_DR] >> 8) & 31; break;
        default: rate = r[F_FEG_DR] & 31; break;
        }
        float target = (float)(r[feg_target[v->feg_state]] & 0x1fff), speed = tables.feg_step[rate];
        if (v->feg_level < target) {
            v->feg_level += speed;
            if (v->feg_level >= target) v->feg_level = target;
        } else if (v->feg_level > target) {
            v->feg_level -= speed;
            if (v->feg_level <= target) v->feg_level = target;
        }
        if (v->feg_level == target && v->feg_state < AICA_EG_DECAY2) ++v->feg_state;
        float f = tables.filter_f[(uint32_t)v->feg_level & 0x1fff];
        float q = tables.filter_damping[r[F_MIX] & 31];
        for (int pass = 0; pass < 2; ++pass) {
            v->filter_low += f * v->filter_band;
            float high = sample - v->filter_low - q * v->filter_band;
            v->filter_band += f * high;
        }
        if (!isfinite(v->filter_low) || fabsf(v->filter_low) > 1e6f) v->filter_low = v->filter_band = 0;
        sample = v->filter_low;
    }

    /* Amplitude envelope. */
    const uint16_t ad = r[F_ENV_AD], dr = r[F_ENV_DR];
    switch (v->eg_state) {
    case AICA_EG_ATTACK: {
        int rate = eg_rate(r, ad & 31);
        float factor = tables.attack_factor[rate];
        if (rate >= 2) v->att = (v->att + 1.0f) * factor - 1.0f;
        if (v->att <= 0.0f) {
            v->att = 0.0f;
            if (!(dr & 0x4000u)) v->eg_state = AICA_EG_DECAY1;
        }
        break;
    }
    case AICA_EG_DECAY1:
        v->att += tables.decay_step[eg_rate(r, (ad >> 6) & 31)];
        if (v->att >= (float)(((dr >> 5) & 31) * 32)) v->eg_state = AICA_EG_DECAY2;
        if (v->att > 1023.0f) v->att = 1023.0f;
        break;
    case AICA_EG_DECAY2:
        v->att += tables.decay_step[eg_rate(r, (ad >> 11) & 31)];
        if (v->att > 1023.0f) v->att = 1023.0f;
        break;
    default:
        v->att += tables.decay_step[eg_rate(r, dr & 31)];
        if (v->att >= 1023.0f) {
            v->att = 1023.0f;
            v->active = false;
        }
        break;
    }
    float gain = (r[F_MIX] & 0x40u) ? 1.0f : tables.eg_gain[(uint32_t)v->att & 1023];
    uint32_t alfos = lfo & 7;
    if (alfos) {
        int wave = tables.alfo_wave[(lfo >> 3) & 3][(v->alfo_phase >> 8) & 255];
        gain *= powf(10.0f, -(alfo_scale_db[alfos] * wave / 256.0f) / 20.0f);
    }
    v->alfo_phase = lfo_reset ? 0 : v->alfo_phase + lfo_step;
    *out = sample * gain * tables.tl_gain[r[F_MIX] >> 8];
    return true;
}

void aica_render(aica_t *chip, int16_t *stereo, uint32_t frames) {
    bool mono = chip->reg[REG(R_COMMON)] & 0x8000u;
    float master = tables.level_gain[chip->reg[REG(R_COMMON)] & 15];
    for (uint32_t frame = 0; frame < frames; ++frame) {
        timers_tick(chip);
        aica_run_cpu(chip, chip->cycles_per_sample);
        float left = 0.0f, right = 0.0f;
        for (uint32_t ch = 0; ch < AICA_VOICES; ++ch) {
            if (!chip->voice[ch].active) continue;
            float sample;
            if (!voice_sample(chip, ch, &sample)) continue;
            const uint16_t *r = CH(chip, ch);
            uint32_t send = r[F_DSP_SEND], direct = r[F_DIRECT];
            uint32_t imxl = (send >> 4) & 15;
            if (imxl) chip->dsp.mixs[send & 15] += (int32_t)(sample * tables.level_gain[imxl] * 16.0f);
            float level = tables.level_gain[(direct >> 8) & 15];
            if (level == 0.0f) continue;
            float pan = mono ? 1.0f : tables.pan_gain[direct & 15];
            bool left_side = (direct & 0x10u) && !mono;
            left += sample * level * (left_side || mono ? 1.0f : pan);
            right += sample * level * (left_side ? pan : 1.0f);
        }
        dsp_step(chip);
        for (uint32_t i = 0; i < 16; ++i) {
            uint32_t efs = chip->reg[REG(R_EFS) + i];
            float level = tables.level_gain[(efs >> 8) & 15];
            if (level == 0.0f || !chip->dsp.efreg[i]) continue;
            float pan = mono ? 1.0f : tables.pan_gain[efs & 15];
            bool left_side = (efs & 0x10u) && !mono;
            float sample = chip->dsp.efreg[i] * level;
            left += sample * (left_side || mono ? 1.0f : pan);
            right += sample * (left_side ? pan : 1.0f);
        }
        left *= master;
        right *= master;
        stereo[frame * 2] = (int16_t)(left < -32768.0f ? -32768 : left > 32767.0f ? 32767 : (int)left);
        stereo[frame * 2 + 1] = (int16_t)(right < -32768.0f ? -32768 : right > 32767.0f ? 32767 : (int)right);
        ++chip->samples;
    }
}
