#include <kos.h>

#include <enDjinn/enj_ctrl.h>
#include <enDjinn/enj_mode.h>
#include <enDjinn/enj_qfont.h>
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_state.h>

#include <aicaflow/bank.h>
#include <aicaflow/host.h>

#include <stdalign.h>
#include <stdbool.h>
#include <stdio.h>

alignas(32) static const unsigned char firmware[] = {
#embed "../../../firmware/aicaflow.drv"
};
alignas(32) static const unsigned char dual[] = {
#embed "../build/dual.afx"
};
alignas(32) static const unsigned char dual_bank[] = {
#embed "../build/dual.afb"
};

static afx_bank_t bank;
static afx_asset_t flow;
static afx_instance_t instance;
static bool assets_loaded, running, finished;
static uint64_t started_ms;
static char message[80] = "Preparing DSP scene";

static void update(void *unused);
static enj_mode_t mode = {.mode_updater = update, .name = "Dual DSP demo"};

static void text(unsigned line, const char *value) {
    enj_qfont_write(value, 14, line * 18, PVR_LIST_PT_POLY);
}

static int wait_recycled(afx_instance_t handle) {
    for (unsigned waited = 0; waited < 2000; ++waited) {
        if (afx_update() < 0) return -AFX_TIMEOUT;
        if (afx_instance_status(handle, &(afx_instance_status_t){0}) == -AFX_STALE_GENERATION) return AFX_OK;
        thd_sleep(1);
    }
    return -AFX_TIMEOUT;
}

static int put(afx_dsp_program_t *program, uint8_t index, afx_dsp_step_t step) {
    if (!step.ysel) step.ysel = AFX_DSP_Y_COEF;
    return afx_dsp_program_step(program, index, &step);
}

/* Two independent paths in one DSP scene: echo returns left and distortion right. */
static int dual_effects_program(afx_dsp_program_t *program) {
    enum { ECHO_DELAY_SAMPLES = 7938, DISTORTION_TEMP = 100 };
    static const int16_t DSP_UNITY = 32760, ECHO_INPUT_GAIN = 8192,
                         ECHO_FEEDBACK = 19656, DISTORTION_OUTPUT_GAIN = 4096;
    int result = afx_dsp_program_init(program);
    /* Steps 1–5 read the 180 ms delay line and return the delayed ACC left. */
    if (!result) result = put(program, 1, (afx_dsp_step_t){.mrd = 1, .nofl = AFX_DSP_MEMORY_AICA_FLOAT, .zero = 1});
    if (!result) result = put(program, 3, (afx_dsp_step_t){.iwt = 1, .iwa = 0, .nofl = AFX_DSP_MEMORY_AICA_FLOAT, .zero = 1});
    if (!result) result = put(program, 4, (afx_dsp_step_t){.ira = AFX_DSP_INPUT_MEMS0, .xsel = 1, .zero = 1});
    if (!result) result = put(program, 5, (afx_dsp_step_t){.ewt = 1, .ewa = AFX_DSP_RETURN_LEFT, .bsel = 1});
    /* Steps 6–9 mix the new MIXS0 click with feedback and write the delay line. */
    if (!result) result = put(program, 6, (afx_dsp_step_t){.ira = AFX_DSP_INPUT_MIXS0, .xsel = 1, .zero = 1});
    if (!result) result = put(program, 7, (afx_dsp_step_t){.ira = AFX_DSP_INPUT_MEMS0, .xsel = 1, .bsel = 1});
    if (!result) result = put(program, 8, (afx_dsp_step_t){.bsel = 1});
    if (!result) result = put(program, 9, (afx_dsp_step_t){.mwt = 1, .nofl = AFX_DSP_MEMORY_AICA_FLOAT, .masa = 1, .zero = 1});
    /* Steps 16–26 repeatedly saturate MIXS1 in TEMP, then return it right. */
    if (!result) result = put(program, 16, (afx_dsp_step_t){.ira = AFX_DSP_INPUT_MIXS1, .xsel = 1, .zero = 1});
    for (uint8_t step = 17; !result && step < 25; step += 2) {
        result = put(program, step, (afx_dsp_step_t){.shift = 1, .twt = 1, .twa = DISTORTION_TEMP, .zero = 1});
        if (!result) result = put(program, step + 1, (afx_dsp_step_t){.tra = DISTORTION_TEMP, .zero = 1});
        if (!result) result = afx_dsp_program_coefficient(program, step + 1, DSP_UNITY);
    }
    if (!result) result = put(program, 25, (afx_dsp_step_t){.tra = DISTORTION_TEMP, .zero = 1});
    if (!result) result = put(program, 26, (afx_dsp_step_t){.ewt = 1, .ewa = AFX_DSP_RETURN_RIGHT, .bsel = 1});
    /* Coefficients and MADRS addresses complete the two independent paths. */
    if (!result) result = afx_dsp_program_coefficient(program, 4, DSP_UNITY);
    if (!result) result = afx_dsp_program_coefficient(program, 6, ECHO_INPUT_GAIN);
    if (!result) result = afx_dsp_program_coefficient(program, 7, ECHO_FEEDBACK);
    if (!result) result = afx_dsp_program_coefficient(program, 16, DSP_UNITY);
    if (!result) result = afx_dsp_program_coefficient(program, 25, DISTORTION_OUTPUT_GAIN);
    if (!result) result = afx_dsp_program_address(program, 0, ECHO_DELAY_SAMPLES);
    if (!result) result = afx_dsp_program_address(program, 1, 0);
    return result;
}

static int start(void) {
    afx_dsp_program_t program;
    int result = dual_effects_program(&program);
    if (!result) result = afx_dsp_scene_program(&program, sizeof(program));
    if (!result) result = afx_instance_activate(flow, &instance);
    if (!result) {
        started_ms = timer_ms_gettime64();
        running = true;
        finished = false;
        snprintf(message, sizeof(message), "Playing: echo, distortion, then both");
    }
    return result;
}

static void render(void *unused) {
    (void)unused;
    uint64_t elapsed = running ? timer_ms_gettime64() - started_ms : 0;
    const char *phase = "FINISHED";
    const char *route = "Press A to replay";
    if (running && elapsed < 2800) {
        phase = "1 / 3  ECHO ONLY";
        route = "MIXS0 clicks  ->  echo  ->  LEFT return";
    } else if (running && elapsed < 5200) {
        phase = "2 / 3  DISTORTION ONLY";
        route = "MIXS1 sine    ->  16x distortion  ->  RIGHT return";
    } else if (running) {
        phase = "3 / 3  BOTH AT ONCE";
        route = "MIXS0 echo LEFT  +  MIXS1 distortion RIGHT";
    }
    enj_qfont_color_set(255, 230, 80);
    text(2, "AICA DSP / TWO INDEPENDENT EFFECTS");
    enj_qfont_color_set(230, 230, 230);
    text(5, phase);
    text(7, route);
    text(10, "One shared 27-step DSP scene; two separate MIXS inputs.");
    text(11, "Dry direct paths are muted: only DSP returns are audible.");
    text(14, message);
    text(17, "A Replay after finish     START+A+B+X+Y Exit");
}

static void cleanup(void) {
    if (running) {
        afx_instance_stop(instance);
        for (unsigned waited = 0; waited < 2000; ++waited) {
            afx_instance_status_t status;
            if (afx_update() || afx_instance_status(instance, &status)) break;
            if (status.state == AFX_DONE || status.state == AFX_ERROR) break;
            thd_sleep(1);
        }
        afx_instance_recycle(instance);
        wait_recycled(instance);
        running = false;
    }
    if (assets_loaded) {
        afx_asset_free(flow);
        afx_bank_release(&bank);
        assets_loaded = false;
    }
    afx_dsp_scene_disable();
}

int main(void) {
    enj_state_init_defaults();
    if (enj_state_startup()) return 1;
    int result = afx_init(firmware, sizeof(firmware));
    if (!result) result = afx_bank_load_memory(&bank, dual_bank, sizeof(dual_bank));
    if (!result) result = afx_bank_flow_upload(&bank, dual, sizeof(dual), &flow);
    if (!result) assets_loaded = true;
    if (!result) result = start();
    if (result || !enj_mode_push(&mode)) {
        cleanup();
        afx_shutdown();
        return 1;
    }
    enj_state_run();
    cleanup();
    afx_shutdown();
    return 0;
}

static void update(void *unused) {
    (void)unused;
    int result = afx_update();
    enj_ctrlr_state_t **states = enj_ctrl_get_states(), *pad = NULL;
    for (size_t index = 0; index < enj_ctrl_states_length() && !pad; ++index) pad = states[index];
    if (!result && running) {
        afx_instance_status_t status;
        if (!afx_instance_status(instance, &status) && (status.state == AFX_DONE || status.state == AFX_ERROR)) {
            result = afx_instance_recycle(instance);
            if (!result) result = wait_recycled(instance);
            if (!result) {
                running = false;
                finished = true;
                snprintf(message, sizeof(message), "Complete: both effects ran in one DSP scene");
            }
        }
    }
    if (!result && pad && finished && pad->button.A == ENJ_BUTTON_DOWN_THIS_FRAME) result = start();
    if (result) snprintf(message, sizeof(message), "DSP demo error (%d)", result);
    enj_render_list_add(PVR_LIST_PT_POLY, render, NULL);
}
