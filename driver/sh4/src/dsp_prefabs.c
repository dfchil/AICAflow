#include <aicaflow/dsp.h>

#include <math.h>
#include <string.h>

static const double pi = 3.14159265358979323846; /* M_PI is not ISO C11. */

/* Public, ready-to-use DSP algorithms and their named presets.
 * Each builder starts a complete program; calling two builders does not chain
 * effects. Step numbers below are MPRO indices executed once per audio sample.
 * Unless stated otherwise, MIXS0 is audio and both EFREG returns carry the same
 * result. EWT/TWT consume the previous ACC while arithmetic computes the next.
 * Unassigned coefficients are zero; BSEL=1 can therefore carry ACC through a
 * write/output step without adding another signal. See dsp.h for field rules. */
static int put(afx_dsp_program_t *program, uint8_t index, afx_dsp_step_t step) {
    /* These prefabs default YSEL to the current step's coefficient. */
    if (!step.ysel) step.ysel = AFX_DSP_Y_COEF;
    return afx_dsp_program_step(program, index, &step);
}

static int put0(afx_dsp_program_t *program, uint8_t index, afx_dsp_step_t step) {
    /* Preserve YSEL=0: interpolation needs the fractional latch, not COEF. */
    return afx_dsp_program_step(program, index, &step);
}

/* MIXS0 -> signed gain -> both returns; a negative gain inverts polarity. */
int afx_dsp_program_gain(afx_dsp_program_t *program, int16_t coefficient) {
    if (coefficient & 7) return -AFX_BAD_COMMAND;
    int result = afx_dsp_program_init(program);
    /* Step 0 multiplies; steps 1 and 2 emit that result left and right. */
    if (!result) result = put(program, 0, (afx_dsp_step_t){.ira = 32, .xsel = 1, .ysel = 1, .zero = 1});
    if (!result) result = put(program, 1, (afx_dsp_step_t){.bsel = 1, .ewt = 1});
    if (!result) result = put(program, 2, (afx_dsp_step_t){.zero = 1, .ewt = 1, .ewa = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 0, coefficient);
    return result;
}

/* MIXS0 -> feedback delay -> both returns. Length is in 44.1 kHz samples;
 * only the delayed signal is returned. NOFL selects the ring's sample format. */
int afx_dsp_program_delay(afx_dsp_program_t *program, uint16_t samples,
                          int16_t feedback, bool linear_memory) {
    if (!samples || feedback < 0 || feedback > 24576 || (feedback & 7)) return -AFX_BAD_COMMAND;
    int result = afx_dsp_program_init(program);
    /* Steps 1/3 fetch and latch the delayed sample; 4–6 return it in stereo. */
    if (!result) result = put(program, 1, (afx_dsp_step_t){.mrd = 1, .nofl = linear_memory, .zero = 1});
    if (!result) result = put(program, 3, (afx_dsp_step_t){.iwt = 1, .iwa = 0, .zero = 1});
    if (!result) result = put(program, 4, (afx_dsp_step_t){.ira = 0, .xsel = 1, .ysel = 1, .zero = 1});
    if (!result) result = put(program, 5, (afx_dsp_step_t){.ewt = 1, .ewa = 0, .bsel = 1});
    /* Step 6 also starts the next write: input/4 + delayed sample * feedback.
     * Step 8 carries the sum so the memory write lands on odd step 9. */
    if (!result) result = put(program, 6, (afx_dsp_step_t){.ira = 32, .xsel = 1, .ysel = 1, .zero = 1, .ewt = 1, .ewa = 1});
    if (!result) result = put(program, 7, (afx_dsp_step_t){.ira = 0, .xsel = 1, .ysel = 1, .bsel = 1});
    if (!result) result = put(program, 8, (afx_dsp_step_t){.bsel = 1});
    if (!result) result = put(program, 9, (afx_dsp_step_t){.mwt = 1, .nofl = linear_memory, .masa = 1, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 4, 32760);
    if (!result) result = afx_dsp_program_coefficient(program, 6, 8192);
    if (!result) result = afx_dsp_program_coefficient(program, 7, feedback);
    /* MASA is the MADRS register index; the uploader handles register spacing. */
    if (!result) result = afx_dsp_program_address(program, 0, samples);
    if (!result) result = afx_dsp_program_address(program, 1, 0);
    return result;
}

/* Two cross-fed delay lines: fresh MIXS0 enters the left line, whose echoes
 * feed the right line and then return to the left. Outputs are wet-only. */
int afx_dsp_program_pingpong(afx_dsp_program_t *program, uint16_t samples,
                             int16_t feedback) {
    if (!samples || feedback < 0 || feedback > 24576 || (feedback & 7)) return -AFX_BAD_COMMAND;
    int result = afx_dsp_program_init(program);
    /* Steps 1–5 read left/right delays into MEMS0/1; 6–9 return each side. */
    if (!result) result = put(program, 1, (afx_dsp_step_t){.mrd = 1, .zero = 1});
    if (!result) result = put(program, 3, (afx_dsp_step_t){.iwt = 1, .iwa = 0, .mrd = 1, .masa = 2, .zero = 1});
    if (!result) result = put(program, 5, (afx_dsp_step_t){.iwt = 1, .iwa = 1, .zero = 1});
    if (!result) result = put(program, 6, (afx_dsp_step_t){.ira = 0, .xsel = 1, .ysel = 1, .zero = 1});
    if (!result) result = put(program, 7, (afx_dsp_step_t){.ewt = 1, .zero = 1});
    if (!result) result = put(program, 8, (afx_dsp_step_t){.ira = 1, .xsel = 1, .ysel = 1, .zero = 1});
    if (!result) result = put(program, 9, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
    /* Steps 10–13 write input/4 + right feedback into the left line;
     * steps 14–15 write left feedback into the right line. */
    if (!result) result = put(program, 10, (afx_dsp_step_t){.ira = 32, .xsel = 1, .ysel = 1, .zero = 1});
    if (!result) result = put(program, 11, (afx_dsp_step_t){.ira = 1, .xsel = 1, .ysel = 1, .bsel = 1});
    if (!result) result = put(program, 12, (afx_dsp_step_t){.bsel = 1});
    if (!result) result = put(program, 13, (afx_dsp_step_t){.mwt = 1, .masa = 1, .zero = 1});
    if (!result) result = put(program, 14, (afx_dsp_step_t){.ira = 0, .xsel = 1, .ysel = 1, .zero = 1});
    if (!result) result = put(program, 15, (afx_dsp_step_t){.mwt = 1, .masa = 3, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 6, 32760);
    if (!result) result = afx_dsp_program_coefficient(program, 8, 32760);
    if (!result) result = afx_dsp_program_coefficient(program, 10, 8192);
    if (!result) result = afx_dsp_program_coefficient(program, 11, feedback);
    if (!result) result = afx_dsp_program_coefficient(program, 14, feedback);
    if (!result) result = afx_dsp_program_address(program, 0, samples);
    if (!result) result = afx_dsp_program_address(program, 1, 0);
    /* The second line's base is 32768 words into the shared ring. */
    if (!result) result = afx_dsp_program_address(program, 2, (uint16_t)(32768u + samples));
    if (!result) result = afx_dsp_program_address(program, 3, 32768);
    return result;
}

/* One-pole smoothing of MIXS0, or its complementary high-pass output.
 * A larger pole retains more history and lowers the low-pass cutoff. */
int afx_dsp_program_lowpass(afx_dsp_program_t *program, int16_t pole, bool highpass) {
    if (pole < 0 || pole > 32000) return -AFX_BAD_COMMAND;
    int16_t feed = (int16_t)(((32768 - pole + 16) / 32) * 8);
    int result = afx_dsp_program_init(program);
    /* Steps 0–1 combine retained TEMP state with scaled input. Both branches
     * store the new low-pass state in TEMP127 for reuse across samples. */
    if (!result) result = put(program, 0, (afx_dsp_step_t){.tra = 0, .zero = 1});
    if (!result) result = put(program, 1, (afx_dsp_step_t){.ira = 32, .xsel = 1, .bsel = 1});
    if (!highpass) {
        if (!result) result = put(program, 2, (afx_dsp_step_t){.twt = 1, .twa = 127, .ewt = 1, .bsel = 1});
        if (!result) result = put(program, 3, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
    } else {
        /* Save low-pass state, then return input/4 minus that state. */
        if (!result) result = put(program, 2, (afx_dsp_step_t){.twt = 1, .twa = 127, .ira = 32, .xsel = 1, .zero = 1});
        if (!result) result = put(program, 3, (afx_dsp_step_t){.tra = 127, .bsel = 1});
        if (!result) result = put(program, 4, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
        if (!result) result = put(program, 5, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
    }
    if (!result) result = afx_dsp_program_coefficient(program, 0, pole);
    if (!result) result = afx_dsp_program_coefficient(program, 1, feed);
    if (highpass && !result) result = afx_dsp_program_coefficient(program, 2, 8192);
    if (highpass && !result) result = afx_dsp_program_coefficient(program, 3, -32768);
    return result;
}

/* MIXS0 audio multiplied by an externally supplied MIXS1 carrier/LFO.
 * Ring modulation is bipolar; tremolo adds an unmodulated component. */
int afx_dsp_program_ringmod(afx_dsp_program_t *program, bool tremolo) {
    int result = afx_dsp_program_init(program);
    if (!tremolo) {
        /* Latch MIXS1 into YREG, multiply MIXS0 by it, then return stereo. */
        if (!result) result = put(program, 0, (afx_dsp_step_t){.ira = 33, .yrl = 1, .zero = 1});
        if (!result) result = put(program, 1, (afx_dsp_step_t){.ira = 32, .xsel = 1, .ysel = 2, .zero = 1});
        if (!result) result = put(program, 2, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
        if (!result) result = put(program, 3, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
        return result;
    }
    /* Steps 0–4 form half-scale audio * (1 + LFO), using TEMP127 as scratch.
     * Steps 5–6 emit the sum to both returns. */
    if (!result) result = put(program, 0, (afx_dsp_step_t){.ira = 33, .yrl = 1, .zero = 1});
    if (!result) result = put(program, 1, (afx_dsp_step_t){.ira = 32, .xsel = 1, .zero = 1});
    if (!result) result = put(program, 2, (afx_dsp_step_t){.twt = 1, .twa = 127, .zero = 1});
    if (!result) result = put(program, 3, (afx_dsp_step_t){.tra = 127, .ysel = 2, .zero = 1});
    if (!result) result = put(program, 4, (afx_dsp_step_t){.ira = 32, .xsel = 1, .bsel = 1});
    if (!result) result = put(program, 5, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
    if (!result) result = put(program, 6, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 1, 16384);
    if (!result) result = afx_dsp_program_coefficient(program, 4, 16384);
    return result;
}

/* MIXS0 -> moving delay tap -> both returns. MIXS1 supplies the modulation;
 * this program does not generate an LFO. Base/depth keep the tap behind the
 * writer. Mixing the return with a dry voice produces chorus/flanging. */
int afx_dsp_program_modulated_delay(afx_dsp_program_t *program, uint16_t base_samples,
                                    int16_t depth, bool interpolate) {
    if (!base_samples || base_samples > 60000 || depth < 0 || depth > 16384 || (depth & 7) ||
        base_samples < (uint16_t)(depth / 16 + 4)) return -AFX_BAD_COMMAND;
    int result = afx_dsp_program_init(program);
    if (interpolate) {
        /* Steps 0–7 split the scaled control into address/fraction latches,
         * then read adjacent ring samples into MEMS0 and MEMS1. */
        if (!result) result = put(program, 0, (afx_dsp_step_t){.ira = 33, .xsel = 1, .zero = 1});
        if (!result) result = put(program, 1, (afx_dsp_step_t){.adrl = 1, .frcl = 1, .shift = 3, .zero = 1});
        if (!result) result = put(program, 3, (afx_dsp_step_t){.mrd = 1, .adreb = 1, .zero = 1});
        if (!result) result = put(program, 5, (afx_dsp_step_t){.iwt = 1, .iwa = 0, .mrd = 1, .adreb = 1, .nxadr = 1, .zero = 1});
        if (!result) result = put(program, 7, (afx_dsp_step_t){.iwt = 1, .iwa = 1, .zero = 1});
        /* Steps 8–11 form sample0 + fraction * (sample1 - sample0).
         * TEMP127 holds the difference; put0 preserves YSEL's fraction mode. */
        if (!result) result = put(program, 8, (afx_dsp_step_t){.ira = 0, .xsel = 1, .zero = 1});
        if (!result) result = put(program, 9, (afx_dsp_step_t){.ira = 1, .xsel = 1, .bsel = 1});
        if (!result) result = put(program, 10, (afx_dsp_step_t){.twt = 1, .twa = 127, .ira = 0, .xsel = 1, .zero = 1});
        if (!result) result = put0(program, 11, (afx_dsp_step_t){.tra = 127, .bsel = 1});
        /* Return the interpolated tap, then write fresh MIXS0/4 at step 15. */
        if (!result) result = put(program, 12, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
        if (!result) result = put(program, 13, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .ira = 32, .xsel = 1, .zero = 1});
        if (!result) result = put(program, 14, (afx_dsp_step_t){.bsel = 1});
        if (!result) result = put(program, 15, (afx_dsp_step_t){.mwt = 1, .masa = 1, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, 0, depth);
        if (!result) result = afx_dsp_program_coefficient(program, 8, -32768);
        if (!result) result = afx_dsp_program_coefficient(program, 9, 32760);
        if (!result) result = afx_dsp_program_coefficient(program, 10, 32760);
        if (!result) result = afx_dsp_program_coefficient(program, 13, 8192);
    } else {
        /* Integer-tap variant: 0–5 select/read one sample, 6–8 return it;
         * step 8 simultaneously scales fresh input for the write at step 9. */
        if (!result) result = put(program, 0, (afx_dsp_step_t){.ira = 33, .xsel = 1, .zero = 1});
        if (!result) result = put(program, 1, (afx_dsp_step_t){.adrl = 1, .shift = 3, .zero = 1});
        if (!result) result = put(program, 3, (afx_dsp_step_t){.mrd = 1, .adreb = 1, .zero = 1});
        if (!result) result = put(program, 5, (afx_dsp_step_t){.iwt = 1, .iwa = 0, .zero = 1});
        if (!result) result = put(program, 6, (afx_dsp_step_t){.ira = 0, .xsel = 1, .zero = 1});
        if (!result) result = put(program, 7, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
        if (!result) result = put(program, 8, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .ira = 32, .xsel = 1, .zero = 1});
        if (!result) result = put(program, 9, (afx_dsp_step_t){.mwt = 1, .masa = 1, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, 0, depth);
        if (!result) result = afx_dsp_program_coefficient(program, 6, 32760);
        if (!result) result = afx_dsp_program_coefficient(program, 8, 8192);
    }
    if (!result) result = afx_dsp_program_address(program, 0, base_samples);
    if (!result) result = afx_dsp_program_address(program, 1, 0);
    return result;
}

/* MIXS0 -> four serial allpass stages -> mix with original -> both returns.
 * MIXS1 supplies their shared moving coefficient; it is not an audio input. */
int afx_dsp_program_phaser(afx_dsp_program_t *program) {
    int result = afx_dsp_program_init(program);
    /* Steps 0–1 keep input/4 in TEMP60 and latch the control into YREG. */
    if (!result) result = put(program, 0, (afx_dsp_step_t){.ira = 32, .xsel = 1, .zero = 1});
    if (!result) result = put(program, 1, (afx_dsp_step_t){.twt = 1, .twa = 60, .ira = 33, .yrl = 1, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 0, 8192);
    /* Each six-step stage uses TEMP50 as scratch and its own output/history
     * at TEMP70, 74, 78 or 82. The next stage consumes the current output. */
    for (uint8_t i = 0; !result && i < 4; ++i) {
        uint8_t b = 2 + i * 6, input = i ? 70 + (i - 1) * 4 : 60, output = 70 + i * 4;
        result = put(program, b, (afx_dsp_step_t){.tra = input, .zero = 1});
        if (!result) result = put(program, b + 1, (afx_dsp_step_t){.tra = output + 1, .bsel = 1});
        if (!result) result = put(program, b + 2, (afx_dsp_step_t){.twt = 1, .twa = 50, .zero = 1});
        if (!result) result = put(program, b + 3, (afx_dsp_step_t){.tra = 50, .ysel = 2, .zero = 1});
        if (!result) result = put(program, b + 4, (afx_dsp_step_t){.tra = input + 1, .bsel = 1});
        if (!result) result = put(program, b + 5, (afx_dsp_step_t){.twt = 1, .twa = output, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, b, 32760);
        if (!result) result = afx_dsp_program_coefficient(program, b + 1, -32768);
        if (!result) result = afx_dsp_program_coefficient(program, b + 4, 32760);
    }
    /* Steps 26–29 mix equal parts final allpass output and original input.
     * Their phase difference creates the notches heard in the mixed signal. */
    if (!result) result = put(program, 26, (afx_dsp_step_t){.tra = 82, .zero = 1});
    if (!result) result = put(program, 27, (afx_dsp_step_t){.tra = 60, .bsel = 1});
    if (!result) result = put(program, 28, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
    if (!result) result = put(program, 29, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 26, 16384);
    if (!result) result = afx_dsp_program_coefficient(program, 27, 16384);
    return result;
}

/* MIXS0 audio with opposing left/right gains controlled by signed MIXS1. */
int afx_dsp_program_autopan(afx_dsp_program_t *program) {
    int result = afx_dsp_program_init(program);
    /* Steps 0–3 latch control and multiply it by input/4 stored in TEMP100. */
    if (!result) result = put(program, 0, (afx_dsp_step_t){.ira = 33, .yrl = 1, .zero = 1});
    if (!result) result = put(program, 1, (afx_dsp_step_t){.ira = 32, .xsel = 1, .zero = 1});
    if (!result) result = put(program, 2, (afx_dsp_step_t){.twt = 1, .twa = 100, .zero = 1});
    if (!result) result = put(program, 3, (afx_dsp_step_t){.tra = 100, .ysel = 2, .zero = 1});
    /* TEMP104 saves the modulation term. Steps 4–5 return base + modulation
     * left; steps 5–7 form base - modulation and return it right. */
    if (!result) result = put(program, 4, (afx_dsp_step_t){.twt = 1, .twa = 104, .tra = 100, .bsel = 1});
    if (!result) result = put(program, 5, (afx_dsp_step_t){.ewt = 1, .tra = 100, .zero = 1});
    if (!result) result = put(program, 6, (afx_dsp_step_t){.tra = 104, .bsel = 1});
    if (!result) result = put(program, 7, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 1, 8192);
    if (!result) result = afx_dsp_program_coefficient(program, 4, 32760);
    if (!result) result = afx_dsp_program_coefficient(program, 5, 32760);
    if (!result) result = afx_dsp_program_coefficient(program, 6, -32768);
    return result;
}

/* Four feed-forward echoes of MIXS0, with alternating stereo placement.
 * One ring writer supplies all taps; no tap is fed back into the writer. */
int afx_dsp_program_multitap(afx_dsp_program_t *program) {
    /* Tap offsets are 70, 130, 220 and 330 ms at 44.1 kHz. */
    static const uint16_t taps[] = {3087, 5733, 9702, 14553};
    int result = afx_dsp_program_init(program);
    if (!result) result = put(program, 0, (afx_dsp_step_t){.ira = 32, .xsel = 1, .zero = 1});
    if (!result) result = put(program, 1, (afx_dsp_step_t){.mwt = 1, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 0, 8192);
    /* Steps 3–17 read the four offsets into MEMS0..3. */
    for (uint8_t i = 0; !result && i < 4; ++i) {
        uint8_t b = 3 + i * 4;
        result = put(program, b, (afx_dsp_step_t){.mrd = 1, .masa = i + 1, .zero = 1});
        if (!result) result = put(program, b + 2, (afx_dsp_step_t){.iwt = 1, .iwa = i, .zero = 1});
        if (!result) result = afx_dsp_program_address(program, i + 1, taps[i]);
    }
    /* Steps 20–25 sum taps 0/2 left and 1/3 right, with quieter later taps. */
    if (!result) result = put(program, 20, (afx_dsp_step_t){.ira = 0, .xsel = 1, .zero = 1});
    if (!result) result = put(program, 21, (afx_dsp_step_t){.ira = 2, .xsel = 1, .bsel = 1});
    if (!result) result = put(program, 22, (afx_dsp_step_t){.ewt = 1, .zero = 1});
    if (!result) result = put(program, 23, (afx_dsp_step_t){.ira = 1, .xsel = 1, .zero = 1});
    if (!result) result = put(program, 24, (afx_dsp_step_t){.ira = 3, .xsel = 1, .bsel = 1});
    if (!result) result = put(program, 25, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 20, 32760);
    if (!result) result = afx_dsp_program_coefficient(program, 21, 16384);
    if (!result) result = afx_dsp_program_coefficient(program, 23, 24576);
    if (!result) result = afx_dsp_program_coefficient(program, 24, 11464);
    return result;
}

/* MIXS0 -> repeated saturating gain -> attenuated stereo return.
 * Each SHIFT=1 stage doubles with saturation: four stages give 16x drive. */
int afx_dsp_program_distortion(afx_dsp_program_t *program, uint8_t stages, bool damped) {
    if (!stages || stages > 6) return -AFX_BAD_COMMAND;
    int result = afx_dsp_program_init(program);
    if (!result) result = put(program, 0, (afx_dsp_step_t){.ira = 32, .xsel = 1, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 0, 32760);
    /* Each pair clips the previous ACC into TEMP100, then reloads it. */
    for (uint8_t i = 0; !result && i < stages; ++i) {
        uint8_t b = 1 + i * 2;
        result = put(program, b, (afx_dsp_step_t){.shift = 1, .twt = 1, .twa = 100, .zero = 1});
        if (!result) result = put(program, b + 1, (afx_dsp_step_t){.tra = 100, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, b + 1, 32760);
    }
    uint8_t b = 1 + stages * 2;
    if (damped) {
        /* Softer overdrive: combine clipped input/16 with half the retained
         * output history, save in TEMP110 and emit to both returns. */
        if (!result) result = put(program, b, (afx_dsp_step_t){.tra = 100, .zero = 1});
        if (!result) result = put(program, b + 1, (afx_dsp_step_t){.tra = 111, .bsel = 1});
        if (!result) result = put(program, b + 2, (afx_dsp_step_t){.twt = 1, .twa = 110, .ewt = 1, .bsel = 1});
        if (!result) result = put(program, b + 3, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, b, 2048);
        if (!result) result = afx_dsp_program_coefficient(program, b + 1, 16384);
    } else {
        /* Hard distortion: return clipped input/8 without smoothing. */
        if (!result) result = put(program, b, (afx_dsp_step_t){.tra = 100, .zero = 1});
        if (!result) result = put(program, b + 1, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
        if (!result) result = put(program, b + 2, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, b, 4096);
    }
    return result;
}

/* Quantize a host-side gain to the DSP's eight-aligned signed coefficient.
 * Positive unity clamps to 32760; exact negative unity fits as -32768. */
static int coefficient(double value, int16_t *out) {
    if (!out || !isfinite(value) || value < -1 || value > 1) return -AFX_BAD_COMMAND;
    long rounded = lround(value * 4096.0) * 8;
    if (rounded < -32768) rounded = -32768;
    if (rounded > 32760) rounded = 32760;
    *out = (int16_t)rounded;
    return AFX_OK;
}

/* Three parallel two-pole resonators. Radius controls decay, frequency the
 * ringing pitch. Normal mode excites them with MIXS0; texture mode uses MIXS1
 * excitation and adds MIXS0 tone plus a little unfiltered MIXS1 at the output. */
int afx_dsp_program_resonators(afx_dsp_program_t *program, const uint16_t frequencies[3],
                               double radius, bool texture) {
    if (!frequencies || radius < .8 || radius > .995) return -AFX_BAD_COMMAND;
    for (uint8_t i = 0; i < 3; ++i) if (frequencies[i] < 150 || frequencies[i] > 8000) return -AFX_BAD_COMMAND;
    int16_t c0, c1, c2;
    int result = afx_dsp_program_init(program);
    /* Steps 0–11 allocate four steps per resonator and separate TEMP histories
     * at 60, 64 and 68. Half-sized feedback coefficients fit the hardware;
     * SHIFT=1 restores their scale when writing each new state. */
    for (uint8_t i = 0; !result && i < 3; ++i) {
        uint8_t b = i * 4, temp = 60 + i * 4;
        result = coefficient((1 - radius) * .125, &c0);
        if (!result) result = coefficient(radius * cos(2 * pi * frequencies[i] / 44100.0), &c1);
        if (!result) result = coefficient(-radius * radius / 2, &c2);
        if (!result) result = put(program, b, (afx_dsp_step_t){.ira = texture ? 33 : 32, .xsel = 1, .zero = 1});
        if (!result) result = put(program, b + 1, (afx_dsp_step_t){.tra = temp + 1, .bsel = 1});
        if (!result) result = put(program, b + 2, (afx_dsp_step_t){.tra = temp + 2, .bsel = 1});
        if (!result) result = put(program, b + 3, (afx_dsp_step_t){.shift = 1, .twt = 1, .twa = temp, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, b, c0);
        if (!result) result = afx_dsp_program_coefficient(program, b + 1, c1);
        if (!result) result = afx_dsp_program_coefficient(program, b + 2, c2);
    }
    /* Steps 12–14 sum the resonators at quarter gain each. */
    for (uint8_t i = 0; !result && i < 3; ++i) {
        result = put(program, 12 + i, (afx_dsp_step_t){.tra = (uint8_t)(60 + i * 4), .zero = i == 0, .bsel = i != 0});
        if (!result) result = afx_dsp_program_coefficient(program, 12 + i, 8192);
    }
    if (texture) {
        /* Add tone/4 and excitation/32 before returning the texture stereo. */
        if (!result) result = put(program, 15, (afx_dsp_step_t){.ira = 32, .xsel = 1, .bsel = 1});
        if (!result) result = put(program, 16, (afx_dsp_step_t){.ira = 33, .xsel = 1, .bsel = 1});
        if (!result) result = put(program, 17, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
        if (!result) result = put(program, 18, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, 15, 8192);
        if (!result) result = afx_dsp_program_coefficient(program, 16, 1024);
    } else {
        if (!result) result = put(program, 15, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
        if (!result) result = put(program, 16, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
    }
    return result;
}

/* MIXS0 -> two moving, interpolated delay taps -> windowed sum -> stereo.
 * External MIXS1/2 ramps move the taps; MIXS3/4 supply their crossfade windows
 * to hide ramp resets. The supplied ramps, not this builder, set pitch ratio. */
int afx_dsp_program_pitch_shift(afx_dsp_program_t *program, bool harmony) {
    int result = afx_dsp_program_init(program);
    /* Each tap gets 16 steps. MEMS0/1 and TEMP120/110 are shared scratch;
     * the windowed tap results are kept separately in TEMP100 and TEMP104. */
    for (uint8_t i = 0; !result && i < 2; ++i) {
        uint8_t b = i * 16, temp = 100 + i * 4;
        result = put(program, b, (afx_dsp_step_t){.ira = 33 + i, .xsel = 1, .zero = 1});
        if (!result) result = put(program, b + 1, (afx_dsp_step_t){.adrl = 1, .frcl = 1, .shift = 3, .zero = 1});
        if (!result) result = put(program, b + 3, (afx_dsp_step_t){.mrd = 1, .adreb = 1, .zero = 1});
        if (!result) result = put(program, b + 5, (afx_dsp_step_t){.iwt = 1, .iwa = 0, .mrd = 1, .adreb = 1, .nxadr = 1, .zero = 1});
        if (!result) result = put(program, b + 7, (afx_dsp_step_t){.iwt = 1, .iwa = 1, .zero = 1});
        /* b+8..11 interpolate sample0 + fraction * (sample1 - sample0). */
        if (!result) result = put(program, b + 8, (afx_dsp_step_t){.ira = 0, .xsel = 1, .zero = 1});
        if (!result) result = put(program, b + 9, (afx_dsp_step_t){.ira = 1, .xsel = 1, .bsel = 1});
        if (!result) result = put(program, b + 10, (afx_dsp_step_t){.twt = 1, .twa = 120, .ira = 0, .xsel = 1, .zero = 1});
        if (!result) result = put0(program, b + 11, (afx_dsp_step_t){.tra = 120, .bsel = 1});
        /* b+12..14 latch this tap's window, multiply and save its contribution. */
        if (!result) result = put(program, b + 12, (afx_dsp_step_t){.twt = 1, .twa = 110, .ira = 35 + i, .yrl = 1, .zero = 1});
        if (!result) result = put(program, b + 13, (afx_dsp_step_t){.tra = 110, .ysel = 2, .zero = 1});
        if (!result) result = put(program, b + 14, (afx_dsp_step_t){.twt = 1, .twa = temp, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, b, 32760);
        if (!result) result = afx_dsp_program_coefficient(program, b + 8, -32768);
        if (!result) result = afx_dsp_program_coefficient(program, b + 9, 32760);
        if (!result) result = afx_dsp_program_coefficient(program, b + 10, 32760);
    }
    /* Steps 32–35 write input/4 into the ring and sum the two windowed taps. */
    if (!result) result = put(program, 32, (afx_dsp_step_t){.ira = 32, .xsel = 1, .zero = 1});
    if (!result) result = put(program, 33, (afx_dsp_step_t){.mwt = 1, .masa = 1, .zero = 1});
    if (!result) result = put(program, 34, (afx_dsp_step_t){.tra = 100, .zero = 1});
    if (!result) result = put(program, 35, (afx_dsp_step_t){.tra = 104, .bsel = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 32, 8192);
    if (!result) result = afx_dsp_program_coefficient(program, 34, 32760);
    if (!result) result = afx_dsp_program_coefficient(program, 35, 32760);
    if (harmony) {
        /* Harmonizer adds the original at 3/16 gain; pitch-shift returns wet. */
        if (!result) result = put(program, 36, (afx_dsp_step_t){.ira = 32, .xsel = 1, .bsel = 1});
        if (!result) result = put(program, 37, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
        if (!result) result = put(program, 38, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, 36, 6144);
    } else {
        if (!result) result = put(program, 36, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
        if (!result) result = put(program, 37, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
    }
    /* Both readers share a 2048-sample base offset; each ramp adds its offset. */
    if (!result) result = afx_dsp_program_address(program, 0, 2048);
    if (!result) result = afx_dsp_program_address(program, 1, 0);
    return result;
}

/* MIXS0 -> biquad -> both returns. Resonant mode selects a low-pass;
 * otherwise frequency/Q/dB define a peaking EQ. Coefficients are designed
 * on the host; only the fixed-point recurrence runs on the AICA DSP. */
int afx_dsp_program_equalizer(afx_dsp_program_t *program, int frequency, double q,
                              double db, bool resonant) {
    if (frequency < 100 || frequency > 15000 || q < .3 || q > 8 || db < -18 || db > 18)
        return -AFX_BAD_COMMAND;
    double w = 2 * pi * frequency / 44100.0, cs = cos(w), alpha = sin(w) / (2 * q);
    double a_gain = pow(10, db / 40), b[3], a[3];
    if (resonant) {
        b[0] = (1 - cs) / 2; b[1] = 1 - cs; b[2] = (1 - cs) / 2;
        a[0] = 1 + alpha; a[1] = -2 * cs; a[2] = 1 - alpha;
    } else {
        b[0] = 1 + alpha * a_gain; b[1] = -2 * cs; b[2] = 1 - alpha * a_gain;
        a[0] = 1 + alpha / a_gain; a[1] = -2 * cs; a[2] = 1 - alpha / a_gain;
    }
    /* Normalize by a0 and halve to fit the coefficient range. Check stability
     * using the quantized feedback terms that the DSP will actually execute. */
    int16_t co[5];
    for (uint8_t i = 0; i < 3; ++i) if (coefficient(b[i] / a[0] / 2, &co[i])) return -AFX_BAD_COMMAND;
    if (coefficient(-a[1] / a[0] / 2, &co[3]) || coefficient(-a[2] / a[0] / 2, &co[4]))
        return -AFX_BAD_COMMAND;
    double a1 = -co[3] / 16384.0, a2 = -co[4] / 16384.0;
    if (fabs(a2) >= 1 || 1 + a1 + a2 <= 0 || 1 - a1 + a2 <= 0) return -AFX_BAD_COMMAND;
    int result = afx_dsp_program_init(program);
    if (!result) result = put(program, 0, (afx_dsp_step_t){.ira = 32, .xsel = 1, .zero = 1});
    if (!result) result = put(program, 1, (afx_dsp_step_t){.twt = 1, .twa = 100, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 0, 8192);
    /* Steps 0–1 save input/4. Steps 2–6 accumulate b0*x + b1*x1 + b2*x2
     * - a1*y1 - a2*y2 from the input and output TEMP histories. */
    static const uint8_t temps[] = {100, 101, 102, 111, 112};
    for (uint8_t i = 0; !result && i < 5; ++i) {
        result = put(program, 2 + i, (afx_dsp_step_t){.tra = temps[i], .zero = i == 0, .bsel = i != 0});
        if (!result) result = afx_dsp_program_coefficient(program, 2 + i, co[i]);
    }
    /* Step 7 doubles the half-scaled sum into output state TEMP110;
     * steps 8–10 reload it and return it left/right. */
    if (!result) result = put(program, 7, (afx_dsp_step_t){.shift = 1, .twt = 1, .twa = 110, .zero = 1});
    if (!result) result = put(program, 8, (afx_dsp_step_t){.tra = 110, .zero = 1});
    if (!result) result = put(program, 9, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
    if (!result) result = put(program, 10, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(program, 8, 32760);
    return result;
}

/* One room diffuser section, placed at MPRO base b. The first takes MIXS0
 * with input attenuation; the next takes TEMP80. Each has its own ring segment
 * and leaves its output in TEMP[temp] for the next stage. */
static int room_allpass(afx_dsp_program_t *p, uint8_t b, uint8_t memory, uint8_t temp,
                        uint16_t base, uint16_t length, bool first, bool large) {
    /* b+1..6 read the delay and form delayed - input/2 (with first-stage
     * input scaling). TWT at b+6 stores that output while starting the sum
     * input + output/2, which b+9 writes back into the delay. */
    int result = put(p, b + 1, (afx_dsp_step_t){.mrd = 1, .masa = memory * 2, .zero = 1});
    if (!result) result = put(p, b + 3, (afx_dsp_step_t){.iwt = 1, .iwa = memory, .zero = 1});
    if (!result) result = put(p, b + 4, (afx_dsp_step_t){.ira = memory, .xsel = 1, .zero = 1});
    if (!result) result = put(p, b + 5, first ? (afx_dsp_step_t){.ira = 32, .xsel = 1, .bsel = 1} :
                                                  (afx_dsp_step_t){.tra = 80, .bsel = 1});
    if (!result) result = put(p, b + 6, first ? (afx_dsp_step_t){.ira = 32, .xsel = 1, .zero = 1, .twt = 1, .twa = temp} :
                                                  (afx_dsp_step_t){.tra = 80, .zero = 1, .twt = 1, .twa = temp});
    if (!result) result = put(p, b + 7, (afx_dsp_step_t){.tra = temp, .bsel = 1});
    if (!result) result = put(p, b + 8, (afx_dsp_step_t){.bsel = 1});
    if (!result) result = put(p, b + 9, (afx_dsp_step_t){.mwt = 1, .masa = memory * 2 + 1, .zero = 1});
    if (!result) result = afx_dsp_program_coefficient(p, b + 4, 32760);
    if (!result) result = afx_dsp_program_coefficient(p, b + 5, first ? -4096 : -16384);
    if (!result) result = afx_dsp_program_coefficient(p, b + 6, first ? 8192 : 32760);
    if (!result) result = afx_dsp_program_coefficient(p, b + 7, 16384);
    /* Each delay uses two MADRS registers: read at base+length, write at base. */
    if (!result) result = afx_dsp_program_address(p, memory * 2, base + length);
    if (!result) result = afx_dsp_program_address(p, memory * 2 + 1, base);
    (void)large;
    return result;
}

/* MIXS0 -> optional predelay -> two diffusers -> parallel damped combs
 * -> weighted stereo returns. Diffuse-only stops after the two diffusers.
 * Feedback sets decay, damping retains low-pass history in each comb, and
 * wet_gain_q8 scales only the final comb mix (256 is unity). */
int afx_dsp_program_room(afx_dsp_program_t *program, int16_t feedback,
                         int16_t damping, bool diffuse_only, int16_t wet_gain_q8, bool large) {
    if (feedback < 0 || feedback > 26208 || (feedback & 7) || damping < 0 || damping > 24576 ||
        (damping & 7) || wet_gain_q8 < 0 || wet_gain_q8 > 512) return -AFX_BAD_COMMAND;
    /* Separate ring segments hold two diffuser lines and four/six comb lines.
     * Lengths/spacing are sample-word offsets, not bytes or AFX timeline ticks. */
    static const uint16_t short_lengths[] = {1153, 1429, 1777, 2089};
    static const uint16_t large_lengths[] = {1559, 2017, 2539, 3011, 3529, 4001};
    const uint16_t *lengths = large ? large_lengths : short_lengths;
    uint8_t combs = large ? 6 : 4;
    uint16_t spacing = large ? 6144 : 8192;
    int result = afx_dsp_program_init(program);
    /* Two 12-step blocks leave the diffused input in TEMP84. Large-room mode
     * shifts both blocks by 12 steps to make space for the predelay. */
    uint8_t offset = large ? 12 : 0;
    result = room_allpass(program, offset, 0, 80, 0, 149, true, large);
    if (!result) result = room_allpass(program, offset + 12, 1, 84, spacing, 211, false, large);
    if (large && !result) {
        /* Steps 1–7 read a 1323-sample (30 ms) predelay into TEMP70 and write
         * fresh input/4. Its ring segment starts after the six comb segments. */
        result = put(program, 1, (afx_dsp_step_t){.mrd = 1, .masa = 18, .zero = 1});
        if (!result) result = put(program, 3, (afx_dsp_step_t){.iwt = 1, .iwa = 10, .zero = 1});
        if (!result) result = put(program, 4, (afx_dsp_step_t){.ira = 10, .xsel = 1, .zero = 1});
        if (!result) result = put(program, 5, (afx_dsp_step_t){.twt = 1, .twa = 70, .ira = 32, .xsel = 1, .zero = 1});
        if (!result) result = put(program, 6, (afx_dsp_step_t){.bsel = 1});
        if (!result) result = put(program, 7, (afx_dsp_step_t){.mwt = 1, .masa = 19, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, 4, 32760);
        if (!result) result = afx_dsp_program_coefficient(program, 5, 8192);
        if (!result) result = afx_dsp_program_address(program, 18, 8 * spacing + 1323);
        if (!result) result = afx_dsp_program_address(program, 19, 8 * spacing);
        /* Patch the first diffuser (base 12) to take predelay TEMP70. */
        if (!result) result = put(program, 17, (afx_dsp_step_t){.tra = 70, .bsel = 1});
        if (!result) result = put(program, 18, (afx_dsp_step_t){.tra = 70, .zero = 1, .twt = 1, .twa = 80});
        if (!result) result = afx_dsp_program_coefficient(program, 17, -16384);
        if (!result) result = afx_dsp_program_coefficient(program, 18, 32760);
    }
    uint8_t comb_start = offset + 24;
    if (diffuse_only) {
        /* Return TEMP84 directly in stereo; no comb tails are constructed. */
        if (!result) result = put(program, comb_start, (afx_dsp_step_t){.tra = 84, .zero = 1});
        if (!result) result = put(program, comb_start + 1, (afx_dsp_step_t){.ewt = 1, .bsel = 1});
        if (!result) result = put(program, comb_start + 2, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, comb_start, 32760);
        return result;
    }
    /* Each 12-step comb block reads its delay, low-passes it using retained
     * TEMP state, then writes diffused input/2 + feedback * filtered delay.
     * The filtered values at TEMP100,104,... also feed the final wet mix. */
    for (uint8_t i = 0; !result && i < combs; ++i) {
        uint8_t b = comb_start + i * 12, memory = i + 2, temp = 100 + i * 4;
        result = put(program, b + 1, (afx_dsp_step_t){.mrd = 1, .masa = memory * 2, .zero = 1});
        if (!result) result = put(program, b + 3, (afx_dsp_step_t){.iwt = 1, .iwa = memory, .zero = 1});
        if (!result) result = put(program, b + 4, (afx_dsp_step_t){.ira = memory, .xsel = 1, .zero = 1});
        if (!result) result = put(program, b + 5, (afx_dsp_step_t){.tra = temp + 1, .bsel = 1});
        if (!result) result = put(program, b + 6, (afx_dsp_step_t){.tra = 84, .zero = 1, .twt = 1, .twa = temp});
        if (!result) result = put(program, b + 7, (afx_dsp_step_t){.tra = temp, .bsel = 1});
        if (!result) result = put(program, b + 8, (afx_dsp_step_t){.bsel = 1});
        if (!result) result = put(program, b + 9, (afx_dsp_step_t){.mwt = 1, .masa = memory * 2 + 1, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, b + 4, (int16_t)(32768 - damping > 32760 ? 32760 : 32768 - damping));
        if (!result) result = afx_dsp_program_coefficient(program, b + 5, damping);
        if (!result) result = afx_dsp_program_coefficient(program, b + 6, 16384);
        if (!result) result = afx_dsp_program_coefficient(program, b + 7, feedback);
        if (!result) result = afx_dsp_program_address(program, memory * 2, (uint16_t)(memory * spacing + lengths[i]));
        if (!result) result = afx_dsp_program_address(program, memory * 2 + 1, memory * spacing);
    }
    /* Alternating 1/4 and 1/8 comb weights give different left/right sums.
     * Apply Q8 wet gain and coefficient alignment before emitting each sum. */
    for (uint8_t channel = 0; !result && channel < 2; ++channel) {
        uint8_t b = comb_start + combs * 12 + channel * (combs + 1);
        for (uint8_t i = 0; !result && i < combs; ++i) {
            int16_t weight = i % 2 == channel ? 8192 : 4096;
            int16_t scaled = (int16_t)(((int32_t)weight * wet_gain_q8 + 1024) / 2048 * 8);
            result = put(program, b + i, (afx_dsp_step_t){.tra = (uint8_t)(100 + i * 4), .zero = i == 0, .bsel = i != 0});
            if (!result) result = afx_dsp_program_coefficient(program, b + i, scaled);
        }
        if (!result) result = put(program, b + combs, (afx_dsp_step_t){.ewt = 1, .ewa = channel, .zero = 1});
    }
    return result;
}

typedef struct {
    const char *name; /* Stable preset name exposed by the preset lookup API. */
    const char *description; /* Short human-readable description of the effect. */
} preset_info_t;
/* Human-readable descriptions correspond to the factory choices below. */
static const preset_info_t presets[] = {
    {"room_large", "30 ms pre-delay, two allpasses, six damped combs, stereo."},
    {"eq_presence", "Peaking EQ: 1400 Hz, +12 dB, Q 1.5; MIXS0."},
    {"resonant_filter", "Resonant lowpass: 900 Hz, Q 5; MIXS0."},
    {"phaser", "Four moving allpasses plus dry; MIXS1 control."},
    {"autopan", "Opposing stereo amplitude modulation; MIXS1 signed LFO."},
    {"pingpong", "180 ms cross-feedback stereo delay; MIXS0."},
    {"multitap", "Four alternating stereo reflections; MIXS0."},
    {"distortion", "16x drive into saturating shifter, attenuated return."},
    {"overdrive", "8x drive with damped output."},
    {"resonators", "Three damped resonators excited by MIXS0."},
    {"bow_texture", "Experimental MIXS1 articulated noise + resonances + MIXS0 tone."},
    {"pitch_shift", "Two interpolated taps; MIXS1..4 ramps/windows required."},
    {"harmonizer", "Shifted signal plus dry; same controls as pitch_shift."},
    {"gain", "Quarter-gain routing baseline; MIXS0 audio."},
    {"invert", "Quarter-gain polarity inversion; MIXS0 audio."},
    {"delay", "50 ms packed-float delay; MIXS0 audio."},
    {"echo", "150 ms echo, 50% feedback; MIXS0 audio."},
    {"lowpass", "One-pole low-pass, pole 26000/32768; MIXS0 audio."},
    {"highpass", "Complementary high-pass; MIXS0 audio."},
    {"diffuser", "Two allpass sections; MIXS0 audio."},
    {"room", "Two allpasses and four damped combs, stereo returns; MIXS0 audio."},
    {"room_warm", "Longer, darker room with stronger wet output; MIXS0 audio."},
    {"ringmod", "MIXS0 audio multiplied by signed MIXS1 carrier."},
    {"tremolo", "MIXS0 audio; signed MIXS1 LFO, output 0.5*(1+LFO)."},
    {"chorus", "Interpolated modulated delay; MIXS0 audio, MIXS1 control waveform."},
    {"flanger", "Short interpolated delay; MIXS0 audio, MIXS1 control waveform."},
};

const char *afx_dsp_program_preset_description(const char *name) {
    if (!name) return NULL;
    for (uint32_t i = 0; i < sizeof(presets) / sizeof(*presets); ++i)
        if (!strcmp(name, presets[i].name)) return presets[i].description;
    return NULL;
}

/* Named parameter sets for the same builders used by runtime callers.
 * Selecting a preset builds one complete image; it does not upload it. */
int afx_dsp_program_preset(afx_dsp_program_t *program, const char *name) {
    static const uint16_t resonator_frequencies[] = {440, 1117, 2311};
    static const uint16_t bow_frequencies[] = {350, 1100, 2400};
    if (!program || !name) return -AFX_BAD_COMMAND;
    if (!strcmp(name, "room_large")) return afx_dsp_program_room(program, 25560, 16384, false, 384, true);
    if (!strcmp(name, "eq_presence")) return afx_dsp_program_equalizer(program, 1400, 1.5, 12, false);
    if (!strcmp(name, "resonant_filter")) return afx_dsp_program_equalizer(program, 900, 5, 12, true);
    if (!strcmp(name, "phaser")) return afx_dsp_program_phaser(program);
    if (!strcmp(name, "autopan")) return afx_dsp_program_autopan(program);
    if (!strcmp(name, "pingpong")) return afx_dsp_program_pingpong(program, 7938, 19656);
    if (!strcmp(name, "multitap")) return afx_dsp_program_multitap(program);
    if (!strcmp(name, "distortion")) return afx_dsp_program_distortion(program, 4, false);
    if (!strcmp(name, "overdrive")) return afx_dsp_program_distortion(program, 3, true);
    if (!strcmp(name, "resonators")) return afx_dsp_program_resonators(program, resonator_frequencies, .985, false);
    if (!strcmp(name, "bow_texture")) return afx_dsp_program_resonators(program, bow_frequencies, .95, true);
    if (!strcmp(name, "pitch_shift")) return afx_dsp_program_pitch_shift(program, false);
    if (!strcmp(name, "harmonizer")) return afx_dsp_program_pitch_shift(program, true);
    if (!strcmp(name, "gain")) return afx_dsp_program_gain(program, 8192);
    if (!strcmp(name, "invert")) return afx_dsp_program_gain(program, -8192);
    if (!strcmp(name, "delay")) return afx_dsp_program_delay(program, 2205, 0, false);
    if (!strcmp(name, "echo")) return afx_dsp_program_delay(program, 6615, 16384, false);
    if (!strcmp(name, "lowpass")) return afx_dsp_program_lowpass(program, 26000, false);
    if (!strcmp(name, "highpass")) return afx_dsp_program_lowpass(program, 26000, true);
    if (!strcmp(name, "diffuser")) return afx_dsp_program_room(program, 22936, 11464, true, 256, false);
    if (!strcmp(name, "room")) return afx_dsp_program_room(program, 22936, 11464, false, 256, false);
    if (!strcmp(name, "room_warm")) return afx_dsp_program_room(program, 25560, 16384, false, 512, false);
    if (!strcmp(name, "ringmod")) return afx_dsp_program_ringmod(program, false);
    if (!strcmp(name, "tremolo")) return afx_dsp_program_ringmod(program, true);
    if (!strcmp(name, "chorus")) return afx_dsp_program_modulated_delay(program, 882, 8192, true);
    if (!strcmp(name, "flanger")) return afx_dsp_program_modulated_delay(program, 256, 2048, true);
    return -AFX_BAD_COMMAND;
}

/* Audition variants: raise selected gains so short demo sounds expose the
 * effect, and give echo a longer decay. The underlying routing is unchanged. */
int afx_dsp_program_demo(afx_dsp_program_t *program, const char *name) {
    /* These are COEF/MPRO indices in the builders above, not bus numbers:
     * room arrays address final wet sums; delay/modulation address ring input.
     * Keep them in sync if the corresponding instruction layouts change. */
    static const uint8_t room[] = {72, 73, 74, 75, 77, 78, 79, 80};
    static const uint8_t large[] = {108, 109, 110, 111, 112, 113, 115, 116, 117, 118, 119, 120};
    static const uint8_t delay[] = {6};
    static const uint8_t pingpong[] = {10};
    static const uint8_t multitap[] = {0};
    static const uint8_t modulation[] = {13};
    const uint8_t *coefficients = NULL;
    uint32_t count = 0;
    if (!program || !name) return -AFX_BAD_COMMAND;
    int result = !strcmp(name, "echo")
        ? afx_dsp_program_delay(program, 6615, 24576, false)
        : afx_dsp_program_preset(program, name);
    if (result) return result;
    if (!strcmp(name, "room") || !strcmp(name, "room_warm")) {
        coefficients = room; count = sizeof(room);
    } else if (!strcmp(name, "room_large")) {
        coefficients = large; count = sizeof(large);
    } else if (!strcmp(name, "delay") || !strcmp(name, "echo")) {
        coefficients = delay; count = sizeof(delay);
    } else if (!strcmp(name, "pingpong")) {
        coefficients = pingpong; count = sizeof(pingpong);
    } else if (!strcmp(name, "multitap")) {
        coefficients = multitap; count = sizeof(multitap);
    } else if (!strcmp(name, "chorus") || !strcmp(name, "flanger")) {
        coefficients = modulation; count = sizeof(modulation);
    }
    for (uint32_t i = 0; i < count; ++i) {
        result = afx_dsp_program_coefficient(program, coefficients[i], 32760);
        if (result) return result;
    }
    return AFX_OK;
}
