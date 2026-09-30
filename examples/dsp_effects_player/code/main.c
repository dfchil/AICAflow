#include <enDjinn/enj_ctrl.h>
#include <enDjinn/enj_mode.h>
#include <enDjinn/enj_qfont.h>
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_state.h>

#include <aicaflow/host.h>
#include <aicaflow/sfx_bank.h>

#include <stdalign.h>
#include <stdio.h>
#include <string.h>

alignas(32) static const unsigned char firmware[] = {
#embed "../../../driver/arm7/aicaflow.drv"
};

typedef struct { const char *name, *preset; } effect_t;
typedef struct { const char *name, *stem; } input_t;
static const effect_t effects[] = {
    {"Delay", "delay"}, {"Echo", "echo"}, {"Diffusion", "diffuser"},
    {"Room", "room"}, {"Warm room", "room_warm"}, {"Low pass", "lowpass"},
    {"High pass", "highpass"}, {"Ring modulation", "ringmod"},
    {"Tremolo", "tremolo"}, {"Chorus", "chorus"}, {"Flanger", "flanger"},
    {"Presence EQ", "eq_presence"}, {"Resonant filter", "resonant_filter"},
    {"Phaser", "phaser"}, {"Ping-pong", "pingpong"}, {"Multitap", "multitap"},
    {"Large room", "room_large"}, {"Autopan", "autopan"},
    {"Overdrive", "overdrive"}, {"Distortion", "distortion"},
    {"Resonators", "resonators"}, {"Pitch shift", "pitch_shift"},
    {"Harmonizer", "harmonizer"}, {"Bow texture", "bow_texture"},
};
static const input_t inputs[] = {
    {"Effect-tuned", "effect"}, {"Impulse", "impulse"}, {"Tone", "tone"},
    {"Modulated tone", "modulated"}, {"Wilhelm scream", "wilhelm"},
};
enum {
    EFFECT_COUNT = sizeof(effects) / sizeof(*effects), INPUT_COUNT = sizeof(inputs) / sizeof(*inputs),
    EFFECT_ROWS = 18,
};

static afx_sfx_bank_t bank;
static afx_asset_t flow;
static afx_instance_t instance;
static int selected, input;
static bool running, returns_enabled = true;
static char message[80] = "Select an effect and press A";

static void update(void *unused);
static enj_mode_t mode = {.mode_updater = update, .name = "DSP effects"};

static void text(unsigned line, const char *value) {
    enj_qfont_write(value, 10, line * 16, PVR_LIST_PT_POLY);
}

static int wait_for(afx_instance_t handle, uint32_t wanted) {
    for (unsigned waited = 0; waited < 2000; ++waited) {
        afx_instance_status_t status;
        if (afx_update() < 0 || afx_instance_status(handle, &status)) return -AFX_TIMEOUT;
        if (status.state == wanted) return AFX_OK;
        if (status.state == AFX_DONE || status.state == AFX_ERROR) return -AFX_BAD_COMMAND;
        thd_sleep(1);
    }
    return -AFX_TIMEOUT;
}

static int wait_recycled(afx_instance_t handle) {
    for (unsigned waited = 0; waited < 2000; ++waited) {
        if (afx_update() < 0) return -AFX_TIMEOUT;
        if (afx_instance_status(handle, &(afx_instance_status_t){0}) == -AFX_STALE_GENERATION) return AFX_OK;
        thd_sleep(1);
    }
    return -AFX_TIMEOUT;
}

static int load_input(const char *stem) {
    char name[48], path[128];
    snprintf(name, sizeof(name), "%s.afb", stem);
    snprintf(path, sizeof(path), "/pc/%s", name);
    int result = afx_sfx_bank_load_samples_file(&bank, path);
    if (result) {
        snprintf(path, sizeof(path), ENJ_CBASEPATH "%s", name);
        result = afx_sfx_bank_load_samples_file(&bank, path);
    }
    if (result) return result;
    snprintf(name, sizeof(name), "%s.afc", stem);
    snprintf(path, sizeof(path), "/pc/%s", name);
    result = afx_sfx_bank_control_upload(&bank, path, &flow);
    if (result) {
        snprintf(path, sizeof(path), ENJ_CBASEPATH "%s", name);
        result = afx_sfx_bank_control_upload(&bank, path, &flow);
    }
    return result;
}

static int unload(void) {
    int result = AFX_OK;
    if (running) {
        afx_instance_status_t status;
        if (afx_instance_status(instance, &status)) result = -AFX_TIMEOUT;
        else if (status.state != AFX_DONE && status.state != AFX_ERROR) {
            result = afx_instance_stop(instance);
            if (!result) result = wait_for(instance, AFX_DONE);
        }
        if (!result) result = afx_instance_recycle(instance);
        if (!result) result = wait_recycled(instance);
        running = false;
    }
    if (!result && flow) result = afx_asset_free(flow);
    flow = AFX_ASSET_INVALID;
    if (!result && bank.sample_data) result = afx_sfx_bank_release(&bank);
    bank = (afx_sfx_bank_t){0};
    if (!result) {
        int scene_result = afx_dsp_scene_disable();
        if (scene_result && scene_result != -AFX_BUSY) return scene_result;
    }
    return result;
}

static int play(void) {
    afx_dsp_program_t program;
    int result = unload();
    if (!result) result = load_input(inputs[input].stem);
    if (!result) result = afx_instance_activate(flow, &instance);
    if (!result) result = wait_for(instance, AFX_RUNNING);
    if (!result) result = afx_dsp_program_demo(&program, effects[selected].preset);
    if (!result) result = afx_dsp_scene_program(&program, sizeof(program));
    if (!result) result = afx_dsp_scene_returns(returns_enabled);
    if (result) {
        unload();
        snprintf(message, sizeof(message), "Could not play effect (%d)", result);
        return result;
    }
    running = true;
    snprintf(message, sizeof(message), "Playing %s with %s", effects[selected].name, inputs[input].name);
    printf("DSP EFFECT: %s / %s\n", effects[selected].preset, inputs[input].stem);
    return AFX_OK;
}

static void render(void *unused) {
    (void)unused;
    enj_qfont_color_set(230, 230, 230);
    text(0, "AICA / DSP EFFECTS PLAYER");
    text(1, returns_enabled ? "Runtime C programs  |  DSP return: wet + dry" :
                               "Runtime C programs  |  DSP return: dry");
    int first = selected - EFFECT_ROWS / 2;
    if (first < 0) first = 0;
    if (first > EFFECT_COUNT - EFFECT_ROWS) first = EFFECT_COUNT - EFFECT_ROWS;
    for (int row = 0; row < EFFECT_ROWS; ++row) {
        int index = first + row;
        char line[48];
        snprintf(line, sizeof(line), "%c %c %-18s %s", index == selected ? '>' : ' ',
                 running && index == selected ? '*' : ' ', effects[index].name, effects[index].preset);
        enj_qfont_color_set(index == selected ? 255 : 210, index == selected ? 230 : 210,
                            index == selected ? 80 : 210);
        text((unsigned)row + 3, line);
    }
    enj_qfont_color_set(230, 230, 230);
    char source[80];
    snprintf(source, sizeof(source), "INPUT: %s", inputs[input].name);
    text(22, source);
    text(23, message);
    text(25, "UP/DOWN Effect  LEFT/RIGHT Input  A Play  B Stop");
    text(26, "Y Wet/Dry     START+A+B+X+Y Exit");
}

int main(void) {
    enj_state_init_defaults();
    if (enj_state_startup()) return 1;
    int result = afx_init(firmware, sizeof(firmware));
    printf("DSP player init=%d\n", result);
    if (result || !enj_mode_push(&mode)) {
        afx_shutdown();
        return 1;
    }
    enj_state_run();
    unload();
    afx_shutdown();
    return 0;
}

static void update(void *unused) {
    (void)unused;
    if (enj_state_get()->flags.shut_down) return;
    int result = afx_update();
    enj_ctrlr_state_t **states = enj_ctrl_get_states(), *pad = NULL;
    for (size_t index = 0; index < enj_ctrl_states_length() && !pad; ++index) pad = states[index];
    if (!result && pad) {
        if (pad->button.UP == ENJ_BUTTON_DOWN_THIS_FRAME) selected = (selected + EFFECT_COUNT - 1) % EFFECT_COUNT;
        if (pad->button.DOWN == ENJ_BUTTON_DOWN_THIS_FRAME) selected = (selected + 1) % EFFECT_COUNT;
        if (pad->button.LEFT == ENJ_BUTTON_DOWN_THIS_FRAME) input = (input + INPUT_COUNT - 1) % INPUT_COUNT;
        if (pad->button.RIGHT == ENJ_BUTTON_DOWN_THIS_FRAME) input = (input + 1) % INPUT_COUNT;
        if (pad->button.A == ENJ_BUTTON_DOWN_THIS_FRAME) result = play();
        if (pad->button.B == ENJ_BUTTON_DOWN_THIS_FRAME) {
            result = unload();
            if (!result) snprintf(message, sizeof(message), "Stopped");
        }
        if (pad->button.Y == ENJ_BUTTON_DOWN_THIS_FRAME) {
            returns_enabled = !returns_enabled;
            if (running) result = afx_dsp_scene_returns(returns_enabled);
        }
    }
    if (!result && running) {
        afx_instance_status_t status;
        if (!afx_instance_status(instance, &status) && (status.state == AFX_DONE || status.state == AFX_ERROR)) {
            bool failed = status.state == AFX_ERROR;
            result = unload();
            if (!result) snprintf(message, sizeof(message), "%s", failed ? "Effect failed" : "Effect finished");
        }
    }
    if (result) snprintf(message, sizeof(message), "DSP player error (%d)", result);
    enj_render_list_add(PVR_LIST_PT_POLY, render, NULL);
}
