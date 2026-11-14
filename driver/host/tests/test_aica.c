/* The software AICA on its own: voices programmed through the registers,
 * measured in the rendered output. */
#include "aica.h"

#include <aicaflow/dsp.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static aica_t chip;
static int16_t out[2 * 44100 * 2];

enum { PCM16 = 0, PCM8 = 1, ADPCM = 2 };

static void reg(uint32_t offset, uint32_t value) { aica_reg_write(&chip, offset, value); }
static void field(uint32_t ch, uint32_t f, uint32_t value) { reg(ch * 0x80u + f * 4u, value); }

static void reset(void) {
    aica_init(&chip);
    reg(0x2800, 0x000f); /* MVOL 0 dB */
}

/* A voice at sample address sa: TL 0, filter off, direct output centred at 0 dB,
 * instant attack, no decay, a fast release. */
static void voice(uint32_t ch, uint32_t format, bool loop, uint32_t sa, uint32_t lsa, uint32_t lea,
                  uint32_t oct, uint32_t fns, uint32_t pan) {
    field(ch, 0, (format << 7) | (loop ? 0x200u : 0) | ((sa >> 16) & 0x7fu));
    field(ch, 1, sa & 0xffffu);
    field(ch, 2, lsa);
    field(ch, 3, lea);
    field(ch, 4, 0x001f);              /* AR 31, D1R 0, D2R 0 */
    field(ch, 5, 0x3c00u | 0x1f);      /* KRS off, DL 0, RR 31 */
    field(ch, 6, (oct & 15) << 11 | fns);
    field(ch, 7, 0);
    field(ch, 8, 0);
    field(ch, 9, 0x0f00u | pan);       /* DISDL 15 */
    field(ch, 10, 0x0020);             /* TL 0, LPOFF */
}

static void key(uint32_t ch, bool on) {
    uint32_t control = chip.reg[ch * 32u];
    field(ch, 0, (on ? control | 0x4000u : control & ~0x4000u) | 0x8000u);
}

static void put16(uint32_t address, int16_t value) {
    chip.ram[address] = (uint8_t)value;
    chip.ram[address + 1] = (uint8_t)((uint16_t)value >> 8);
}

/* Frequency from rising zero crossings of one channel. */
static double frequency(const int16_t *stereo, uint32_t frames, int channel) {
    int first = -1, last = -1, crossings = 0;
    for (uint32_t i = 1; i < frames; ++i) {
        if (stereo[(i - 1) * 2 + channel] < 0 && stereo[i * 2 + channel] >= 0) {
            if (first < 0) first = (int)i;
            last = (int)i;
            ++crossings;
        }
    }
    return crossings > 1 ? (crossings - 1) * (double)AICA_RATE / (last - first) : 0;
}

static double rms(const int16_t *stereo, uint32_t frames, int channel) {
    double sum = 0;
    for (uint32_t i = 0; i < frames; ++i) sum += (double)stereo[i * 2 + channel] * stereo[i * 2 + channel];
    return sqrt(sum / frames);
}

static void test_pitch(void) {
    /* One period of a sine over 100 samples: 441 Hz at a step of 1.0. */
    const struct { uint32_t format, oct, fns; double hz; } cases[] = {
        {PCM16, 0, 0, 441.0}, {PCM16, 1, 0, 882.0}, {PCM16, 15, 0, 220.5},
        {PCM16, 0, 512, 661.5}, {PCM8, 0, 0, 441.0}};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(*cases); ++c) {
        reset();
        for (int i = 0; i < 100; ++i) {
            int16_t s = (int16_t)lrint(16000 * sin(2 * acos(-1.0) * i / 100.0));
            if (cases[c].format == PCM16) put16(0x10000u + i * 2u, s);
            else chip.ram[0x10000u + i] = (uint8_t)(s >> 8);
        }
        voice(0, cases[c].format, true, 0x10000, 0, 100, cases[c].oct, cases[c].fns, 0);
        key(0, true);
        aica_render(&chip, out, 44100);
        double hz = frequency(out, 44100, 0);
        printf("pitch %s oct %u fns %u: %.2f Hz (want %.2f), rms %.0f\n",
               cases[c].format == PCM16 ? "pcm16" : "pcm8", cases[c].oct, cases[c].fns, hz,
               cases[c].hz, rms(out, 44100, 0));
        assert(fabs(hz - cases[c].hz) < 0.5);
        assert(fabs(rms(out, 44100, 0) - 16000 / sqrt(2.0)) < 400);
    }
}

static void test_pan(void) {
    reset();
    for (int i = 0; i < 100; ++i) put16(0x10000u + i * 2u, (int16_t)lrint(16000 * sin(2 * acos(-1.0) * i / 100.0)));
    voice(0, PCM16, true, 0x10000, 0, 100, 0, 0, 0x1f); /* bit 4: the right side is attenuated */
    key(0, true);
    aica_render(&chip, out, 4410);
    printf("pan 0x1f: left %.0f right %.0f\n", rms(out, 4410, 0), rms(out, 4410, 1));
    assert(rms(out, 4410, 0) > 10000 && rms(out, 4410, 1) < 1);
    field(0, 9, 0x0f00 | 0x06); /* left 18 dB down */
    aica_render(&chip, out, 4410);
    double ratio = 20 * log10(rms(out, 4410, 0) / rms(out, 4410, 1));
    printf("pan 0x06: left/right %.1f dB\n", ratio);
    assert(fabs(ratio + 18.0) < 0.2);
}

static void test_one_shot_and_release(void) {
    reset();
    for (int i = 0; i < 1000; ++i) put16(0x10000u + i * 2u, 8000);
    voice(0, PCM16, false, 0x10000, 0, 1000, 0, 0, 0);
    key(0, true);
    aica_render(&chip, out, 1200);
    int last = -1;
    for (int i = 0; i < 1200; ++i) if (out[i * 2]) last = i;
    printf("one-shot of 1000 samples: last sound at %d, voice %s\n", last, chip.voice[0].active ? "on" : "off");
    assert(last >= 995 && last <= 1000 && !chip.voice[0].active);

    /* Release at RR 16 is rate 32 with key scaling off: 690 ms for the full
     * 96 dB in MAME's table, after which the voice stops. */
    reset();
    for (int i = 0; i < 100; ++i) put16(0x10000u + i * 2u, (int16_t)lrint(16000 * sin(2 * acos(-1.0) * i / 100.0)));
    voice(0, PCM16, true, 0x10000, 0, 100, 0, 0, 0);
    field(0, 5, 0x3c00u | 16);
    key(0, true);
    aica_render(&chip, out, 4410);
    key(0, false);
    uint32_t frames = 0;
    while (chip.voice[0].active && frames < 44100) { aica_render(&chip, out, 1); ++frames; }
    printf("release RR 16: voice off after %.0f ms (want 690)\n", frames * 1000.0 / AICA_RATE);
    assert(fabs(frames * 1000.0 / AICA_RATE - 690) < 5);
}

/* The decoder AICAforge's encoder targets (src/afx_sample_c.c). */
static void adpcm_reference(const uint8_t *data, uint32_t frames, int16_t *decoded) {
    static const int diff[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
    static const int scale[8] = {230, 230, 230, 230, 307, 409, 512, 614};
    int current = 0, quant = 127;
    for (uint32_t i = 0; i < frames; ++i) {
        int code = (data[i >> 1] >> ((i & 1u) * 4u)) & 15;
        current += quant * diff[code] / 8;
        if (current < -32768) current = -32768; else if (current > 32767) current = 32767;
        quant = quant * scale[code & 7] >> 8;
        if (quant < 127) quant = 127; else if (quant > 24576) quant = 24576;
        decoded[i] = (int16_t)current;
    }
}

static void test_adpcm(void) {
    reset();
    enum { FRAMES = 4000 };
    static int16_t want[FRAMES];
    uint32_t seed = 12345;
    for (int i = 0; i < FRAMES / 2; ++i) {
        seed = seed * 1103515245u + 12345u;
        /* Mostly small steps, so the predictor stays in range like real audio. */
        uint8_t lo = (seed >> 16) & 0x0b, hi = (seed >> 24) & 0x0b;
        chip.ram[0x20000 + i] = (uint8_t)(lo | hi << 4);
    }
    adpcm_reference(chip.ram + 0x20000, FRAMES, want);
    voice(0, ADPCM, false, 0x20000, 0, FRAMES, 0, 0, 0);
    key(0, true);
    aica_render(&chip, out, FRAMES);
    int worst = 0;
    for (int i = 0; i < FRAMES - 1; ++i) {
        int error = abs(out[i * 2] - want[i]);
        if (error > worst) worst = error;
    }
    printf("adpcm: %d samples, largest difference from AICAforge's decoder %d\n", FRAMES, worst);
    assert(worst <= 1);
}

static void test_dsp_delay(void) {
    reset();
    afx_dsp_program_t program;
    assert(afx_dsp_program_delay(&program, 4410, 0, true) == AFX_OK);
    for (int i = 0; i < AFX_DSP_MPRO_WORDS; ++i) reg(0x3400u + i * 4u, program.words[i]);
    for (int i = 0; i < AFX_DSP_COEFFICIENTS; ++i) reg(0x3000u + i * 4u, program.words[AFX_DSP_MPRO_WORDS + i]);
    for (int i = 0; i < AFX_DSP_ADDRESSES; ++i)
        reg(0x3200u + i * 4u, program.words[AFX_DSP_MPRO_WORDS + AFX_DSP_COEFFICIENTS + i]);
    reg(0x2000, program.words[AFX_DSP_PROGRAM_WORDS - 2]);
    reg(0x2004, program.words[AFX_DSP_PROGRAM_WORDS - 1]);
    reg(0x2804, (3u << 13) | (0x100000u >> 11)); /* 64K-word ring at 1 MiB */
    for (int i = 0; i < 32; ++i) put16(0x10000u + i * 2u, 20000);
    voice(0, PCM16, false, 0x10000, 0, 32, 0, 0, 0);
    field(0, 8, 0x00f0); /* IMXL 15 to MIXS0 */
    field(0, 9, 0x0000); /* no direct output */
    key(0, true);
    aica_render(&chip, out, 8820);
    int first = -1, peak = 0;
    for (int i = 0; i < 8820; ++i) {
        int v = abs(out[i * 2]) > abs(out[i * 2 + 1]) ? abs(out[i * 2]) : abs(out[i * 2 + 1]);
        if (v > 50 && first < 0) first = i;
        if (v > peak) peak = v;
    }
    printf("dsp delay of 4410: first output at %d, peak %d\n", first, peak);
    assert(first >= 4405 && first <= 4415 && peak > 1000);
}

int main(void) {
    test_pitch();
    test_pan();
    test_one_shot_and_release();
    test_adpcm();
    test_dsp_delay();
    puts("software AICA checks passed");
    return 0;
}
