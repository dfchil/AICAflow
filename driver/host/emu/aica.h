/* A software model of the Dreamcast's AICA sound chip with its ARM7: RAM,
 * registers, timers, interrupts, 64 voices, the DSP and the mixer. One
 * instance is the whole chip. Not thread-safe; callers serialise access. */
#ifndef AICAFLOW_HOST_AICA_H
#define AICAFLOW_HOST_AICA_H

#include <stdbool.h>
#include <stdint.h>

#include "arm7.h"

#define AICA_RAM_BYTES 0x200000u
#define AICA_REG_BYTES 0x8000u
#define AICA_RATE 44100u
#define AICA_VOICES 64u

typedef struct {
    bool active;
    uint8_t eg_state;      /* AICA_EG_* */
    bool lpend;            /* The loop end was reached since the monitor last read it. */
    float att;             /* Envelope attenuation, 0 (loud) to 1023 (silent), 3/32 dB a step. */
    uint32_t position;     /* Sample index with a 12-bit fraction. */
    /* ADPCM decoder: nibbles decoded so far, the last two samples, the predictor. */
    uint32_t adpcm_decoded;
    int32_t adpcm_previous, adpcm_current, adpcm_quant;
    int32_t loop_signal, loop_quant;
    bool loop_saved;
    uint32_t noise;
    uint32_t plfo_phase, alfo_phase; /* 8.8 fixed point over a 256-entry table. */
    uint8_t feg_state;
    float feg_level;
    float filter_low, filter_band;
} aica_voice_t;

typedef struct {
    int32_t temp[128], mems[32], mixs[16];
    int16_t efreg[16];
    uint32_t dec;
    int last_step;
    bool program_dirty;
} aica_dsp_t;

typedef struct aica {
    uint8_t ram[AICA_RAM_BYTES];
    uint16_t reg[AICA_REG_BYTES / 4]; /* Register words at offset / 4, low 16 bits. */
    aica_voice_t voice[AICA_VOICES];
    aica_dsp_t dsp;
    arm7_t arm;
    bool arm_reset;            /* Held in reset, as after power-on. */
    uint32_t timer_count[3];   /* 8.8: counter in the high byte. */
    bool timer_expired[3];
    uint32_t scipd, mcipd;
    uint32_t lfo_noise;
    uint32_t cycles_per_sample;
    uint64_t samples;
} aica_t;

void aica_init(aica_t *chip);
/* Holds the ARM7 in reset, or releases it to boot from address 0. */
void aica_arm_reset(aica_t *chip, bool held);
/* Keys every voice off, as KOS's spu_disable does. */
void aica_key_off_all(aica_t *chip);
/* Register access at chip offsets 0x0000-0x7fff (SH4 0xa0700000 + offset,
 * ARM7 0x800000 + offset). Only the low 16 bits of a register are kept. */
uint32_t aica_reg_read(aica_t *chip, uint32_t offset);
void aica_reg_write(aica_t *chip, uint32_t offset, uint32_t value);
/* Runs the ARM7 without advancing time. */
void aica_run_cpu(aica_t *chip, uint32_t instructions);
/* Produces interleaved stereo at 44.1 kHz, running the ARM7, the timers, the
 * voices and the DSP one sample at a time. */
void aica_render(aica_t *chip, int16_t *stereo, uint32_t frames);
/* The SH4 interrupt line: MCIPD & MCIEB. */
bool aica_main_irq(const aica_t *chip);

#endif
