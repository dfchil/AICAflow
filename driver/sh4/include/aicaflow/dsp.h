#ifndef AICAFLOW_DSP_H
#define AICAFLOW_DSP_H

#include <stdbool.h>
#include <stdint.h>

#include <aicaflow_protocol.h>

/*
 * AICA DSP program authoring API
 *
 * The AICA executes all 128 MPRO steps once for every 44.1 kHz audio sample.
 * There is no branching or variable execution time: an unused step must still
 * be harmless.  afx_dsp_program_init() fills every step with safe idle
 * arithmetic (COEF * TEMP + 0, with a zero coefficient and no write enabled).
 *
 * A program has four distinct regions, in exactly the wire order accepted by
 * afx_dsp_scene_program():
 *
 *   MPRO[128][4]  micro-instructions       512 u16 / 1024 bytes
 *   COEF[128]     one signed Q1.15-ish coefficient per step
 *   MADRS[64]     external delay-memory word offsets
 *   EFREG[2]      stereo return-control words
 *
 * Keep the image in this word form while authoring on SH-4. It is already
 * little-endian and can be passed directly to afx_dsp_scene_program(). Do not
 * send individual AICA registers yourself: that API performs the required
 * mute, clear, memory-format initialization and ordered MMIO writes.
 *
 * Minimal custom effect (quarter-scale MIXS0 on both effect returns):
 *
 *   afx_dsp_program_t dsp;
 *   afx_dsp_step_t step = {0};
 *   afx_dsp_program_init(&dsp);
 *
 *   // ACC = MIXS0 * COEF[0]. IRA 32 selects MIXS0.
 *   step.ira = 32; step.xsel = 1; step.ysel = 1; step.zero = 1;
 *   afx_dsp_program_step(&dsp, 0, &step);
 *   afx_dsp_program_coefficient(&dsp, 0, 8192); // approximately 1/4
 *
 *   // EWT writes the *previous* ACC, so output starts on the next step.
 *   step = (afx_dsp_step_t){ .ysel = 1, .bsel = 1, .ewt = 1, .ewa = 0 };
 *   afx_dsp_program_step(&dsp, 1, &step);
 *   step.ewa = 1;
 *   afx_dsp_program_step(&dsp, 2, &step);
 *
 *   afx_dsp_scene_program(&dsp, sizeof(dsp));
 *
 * Start from one of the named factories whenever it fits. For a new effect,
 * first prove a dry source, then gain, one output, one delay tap and only then
 * feedback or modulation. A valid encoding is not proof of stable or musical
 * audio; use an impulse/short burst and hardware capture for a new algorithm.
 */
enum {
    AFX_DSP_STEPS = 128,
    AFX_DSP_MPRO_WORDS = AFX_DSP_STEPS * 4,
    AFX_DSP_COEFFICIENTS = 128,
    AFX_DSP_ADDRESSES = 64,
    AFX_DSP_PROGRAM_WORDS = AFX_DSP_MPRO_WORDS + AFX_DSP_COEFFICIENTS +
                            AFX_DSP_ADDRESSES + 2,
    AFX_DSP_PROGRAM_BYTES = AFX_DSP_PROGRAM_WORDS * sizeof(uint16_t)
};

typedef struct { uint16_t words[AFX_DSP_PROGRAM_WORDS]; } afx_dsp_program_t;

/*
 * One logical MPRO instruction. Fields map directly to the four hardware u16
 * words; reserved bits are not exposed. afx_dsp_program_step() replaces the
 * whole instruction, so set every field needed by the step. In particular,
 * set ysel=1 explicitly when multiplying by that step's coefficient.
 *
 * Arithmetic is conceptually:
 *
 *   next_ACC = selected_X * selected_Y + selected_B
 *
 * XSEL=0 selects TEMP[TRA]; XSEL=1 selects the input named by IRA. BSEL=0
 * selects TEMP[TRA] as B; BSEL=1 selects the previous ACC as B. ZERO=1 makes
 * B zero. NEGB negates B. Crucially, TWT/EWT/SHIFT consume the *previous* ACC
 * while the instruction computes next_ACC. Therefore compute at step N and
 * write/output it at N+1 or later; do not expect an EWT in the same step to
 * write the multiplication just described.
 *
 * IRA: 0..31 reads MEMS0..31; 32..47 reads MIXS0..15. A normal audio effect
 * reads the source voice's bus with IRA=32 (MIXS0). Values beyond 47 are
 * special hardware inputs; use a named/calibrated factory before relying on
 * them.
 *
 * YSEL: 0 = fractional latch, 1 = COEF[step], 2/3 = YREG portions. The common
 * case is 1. YRL loads YREG from the selected input, then a later step can use
 * YSEL=2 for ring modulation, tremolo or other calibrated control input.
 *
 * TEMP: TWT writes the selected shifted previous ACC to TEMP[TWA]. TEMP's
 * logical addresses rotate by one every audio sample: a value written to
 * TEMP127 is visible as TEMP0 next sample. This is why the one-pole factory
 * writes 127 and reads 0. TEMP is good for state/short histories, not a room.
 *
 * External delay memory: MRD/MWT use MADRS[MASA] and are permitted only on
 * odd step indices (1, 3, ... 127). A validated read sequence is MRD at step
 * 1, IWT at step 3, then arithmetic reads MEMS at step 4. IWT does not make a
 * just-read word available in the same instruction. MADRS values are word
 * offsets, never byte offsets. TABLE is intentionally rejected by Aicaflow.
 *
 * NOFL must be identical on every memory-access step in one program:
 *   0: packed AICA float; ring silence is 0x6000 (the default room/delay)
 *   1: signed linear sample; zero-filled ring is silent
 * The scene uploader picks its clear value from this bit. Mixing formats would
 * make at least one delay path start with invalid data, so it is rejected.
 *
 * EWT writes an effect return. Aicaflow permits EWA 0 (EFREG0/left) and 1
 * (EFREG1/right) only. Build a sum in ACC/TEMP first; repeated EWT writes do
 * not imply accumulation. Return-control words route/mix EFREG after this
 * program; they are not DSP instructions.
 */
typedef struct {
    /* W0: TEMP read/write addresses. TRA/TWA are 0..127. */
    uint8_t tra, twt, twa;
    /* W1: operand selection. XSEL/YSEL/IWT are single-bit except YSEL 0..3. */
    uint8_t xsel, ysel, ira, iwt, iwa;
    /* W2: external access, output, accumulator shaping and arithmetic B. */
    uint8_t table, mwt, mrd, ewt, ewa;
    uint8_t adrl, frcl, shift, yrl, negb, zero, bsel;
    /* W3: external-memory format/address controls. */
    uint8_t nofl, masa, adreb, nxadr;
} afx_dsp_step_t;

/*
 * Starts with 128 safe idle steps and normal stereo return routing
 * (EFREG0=0x0f1f, EFREG1=0x0f0f). It does not allocate or touch AICA RAM.
 * Call once before individual setters; factory functions call it themselves.
 * Every public builder returns AFX_OK or -AFX_BAD_COMMAND and leaves the
 * program suitable for afx_dsp_scene_program() only on AFX_OK.
 */
int afx_dsp_program_init(afx_dsp_program_t *program);

/*
 * Encodes one complete step at index 0..127. Field ranges, odd memory steps,
 * TABLE=0, uniform NOFL and EWA <= 1 are checked here. This routine does not
 * insert defaults: use an explicit .ysel = 1 for ordinary coefficient math.
 */
int afx_dsp_program_step(afx_dsp_program_t *program, uint8_t index,
                         const afx_dsp_step_t *step);

/*
 * COEF[index] is signed and must have its low three bits clear. 8192 is about
 * +0.25, 16384 about +0.5, 32760 is the largest positive near-unity value and
 * -32768 is exact negative unity. Each instruction with YSEL=1 reads the
 * coefficient at its own step index; there is no separate coefficient address.
 */
int afx_dsp_program_coefficient(afx_dsp_program_t *program, uint8_t index,
                                int16_t value);

/*
 * Sets MADRS[index], a non-negative 16-bit word offset into the configured
 * 128 KiB delay ring. Account for every read/write window before sharing the
 * ring between delays; separate writer bases alone do not prevent overwrite.
 */
int afx_dsp_program_address(afx_dsp_program_t *program, uint8_t index,
                            uint16_t value);

/*
 * Sets EFREG0/1 return-control words. Only bits 0..12 are accepted. The
 * defaults are the measured stereo routing. Set both words to zero for a
 * deliberately silent program. afx_dsp_scene_returns(true) only restores the
 * words stored at upload time, so it cannot make a zero-return image audible.
 * Return gating never clears the program's internal delay state.
 */
int afx_dsp_program_returns(afx_dsp_program_t *program, uint16_t left,
                            uint16_t right);

/*
 * Runtime-safe building blocks. Each overwrites program completely, validates
 * its parameter range and leaves the regular default stereo returns installed.
 *
 * delay/pingpong samples are 44.1 kHz delay-ring samples, not AFX ticks.
 * feedback is a non-negative aligned coefficient and is capped below unstable
 * unity. linear_memory=true selects NOFL=1; false selects packed AICA float.
 * Room wet gain is Q8: 256=1.0, 384=1.5 and 512=2.0. Modulated-delay control
 * arrives on MIXS1 and must remain safely behind the writer over its complete
 * range. The EQ/resonator factories validate quantization/stability but still
 * require hardware headroom and decay checks for any new parameter choice.
 */
int afx_dsp_program_gain(afx_dsp_program_t *program, int16_t coefficient);
int afx_dsp_program_delay(afx_dsp_program_t *program, uint16_t samples,
                          int16_t feedback, bool linear_memory);
int afx_dsp_program_pingpong(afx_dsp_program_t *program, uint16_t samples,
                             int16_t feedback);
int afx_dsp_program_lowpass(afx_dsp_program_t *program, int16_t pole, bool highpass);
int afx_dsp_program_room(afx_dsp_program_t *program, int16_t feedback,
                         int16_t damping, bool diffuse_only, int16_t wet_gain_q8,
                         bool large);
int afx_dsp_program_ringmod(afx_dsp_program_t *program, bool tremolo);
int afx_dsp_program_modulated_delay(afx_dsp_program_t *program, uint16_t base_samples,
                                    int16_t depth_coefficient, bool interpolate);
int afx_dsp_program_equalizer(afx_dsp_program_t *program, int frequency, double q,
                              double db, bool resonant);
int afx_dsp_program_phaser(afx_dsp_program_t *program);
int afx_dsp_program_autopan(afx_dsp_program_t *program);
int afx_dsp_program_multitap(afx_dsp_program_t *program);
int afx_dsp_program_distortion(afx_dsp_program_t *program, uint8_t stages, bool damped);
int afx_dsp_program_resonators(afx_dsp_program_t *program, const uint16_t frequencies[3],
                               double radius, bool texture);
int afx_dsp_program_pitch_shift(afx_dsp_program_t *program, bool harmony);

/*
 * Named, parameter-free offline presets. The C CLI uses these exact factories:
 *
 *   make dsp-tool
 *   build/afx_dsp_program room build/my-room.dsp
 *
 * The CLI is deliberately only a serializer; no Python generator or alternate
 * DSP encoding exists. Pass the same constructed object to
 * afx_dsp_scene_program() for runtime-defined DSP. Unknown names return
 * -AFX_BAD_COMMAND; use afx_dsp_program_preset_description() to discover a
 * known name or show its authoring intent in a UI.
 */
int afx_dsp_program_preset(afx_dsp_program_t *program, const char *name);
const char *afx_dsp_program_preset_description(const char *name);

/* Builds the level-calibrated variant used by dsp_effects_player. It is a
 * demonstration profile, not a new general-purpose effect API: use preset()
 * or the parameterized builders for authored programs. */
int afx_dsp_program_demo(afx_dsp_program_t *program, const char *name);

#endif /* AICAFLOW_DSP_H */
