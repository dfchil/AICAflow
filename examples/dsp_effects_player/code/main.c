#include <enDjinn/enj_ctrl.h>
#include <enDjinn/enj_mode.h>
#include <enDjinn/enj_qfont.h>
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_state.h>

#include <aicaflow/bank.h>
#include <aicaflow/host.h>

#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* KOS ISO9660 streams aligned reads. Its asynchronous sub-32-byte tail
 * can stall in Flycast; finish the aligned body, then seek to stop that
 * stream before reading the tail through the sector cache. Files using
 * this helper must be seekable and opened with _IONBF. */
static size_t asset_read(void *buffer, size_t bytes, FILE *file) {
    size_t body = bytes & ~(size_t)31;
    size_t got = body ? fread(buffer, 1, body, file) : 0;
    if (got != body || bytes == body) return got;
    long position = ftell(file);
    if (position < 0) return got;
    if (position && (fseek(file, position - 1, SEEK_SET) ||
                     fseek(file, position, SEEK_SET))) return got;
    return got + fread((uint8_t *)buffer + got, 1, bytes - got, file);
}

alignas(32) static const unsigned char firmware[] = {
#embed "../../../firmware/aicaflow.drv"
};

enum { CONTROL_PHRASE, IMPULSE, TONE, MODULATED, WILHELM, SLOW };
typedef struct { const char *name, *preset; unsigned input; } effect_t;
typedef struct { const char *name, *stem; } input_t;
static const effect_t effects[] = {
    {"Echo", "echo", IMPULSE}, {"Room", "room", IMPULSE},
    {"Warm room", "room_warm", IMPULSE}, {"Chorus", "chorus", SLOW},
    {"Flanger", "flanger", SLOW}, {"Resonant filter", "resonant_filter", WILHELM},
    {"Ping-pong", "pingpong", IMPULSE}, {"Multitap", "multitap", IMPULSE},
    {"Large room", "room_large", IMPULSE},
};
static const input_t inputs[] = {
    {"Control phrase", "effect"}, {"Impulse", "impulse"}, {"Tone", "tone"},
    {"Modulated tone", "modulated"}, {"Wilhelm scream", "wilhelm"},
    {"Wilhelm + slow LFO", "slow"},
};
enum {
    EFFECT_COUNT = sizeof(effects) / sizeof(*effects), INPUT_COUNT = sizeof(inputs) / sizeof(*inputs),
    EFFECT_ROWS = EFFECT_COUNT < 18 ? EFFECT_COUNT : 18,
};

static afx_bank_t bank;
static afx_asset_t flows[INPUT_COUNT];
static afx_instance_t instance;
/* input == -1 selects the effect's suggested source; other values are manual. */
static int selected, input = -1, playing_effect = -1, installed_effect = -1;
static bool running, returns_enabled = true;
static char message[80] = "Select an effect and press A";

static unsigned selected_input(void) {
    return input < 0 ? effects[selected].input : (unsigned)input;
}

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

static int load_flow_file(const char *path, afx_asset_t *flow) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) { if (file) fclose(file); return -AFX_BAD_BOUNDS; }
    long length = ftell(file);
    if (length <= 0 || (uint64_t)length > UINT32_MAX || fseek(file, 0, SEEK_SET)) {
        fclose(file); return -AFX_BAD_BOUNDS;
    }
    uint8_t *data = malloc((size_t)length);
    int result = -AFX_NO_HOST_RAM;
    if (data && asset_read(data, (size_t)length, file) == (size_t)length && !ferror(file))
        result = afx_bank_flow_upload(&bank, data, (uint32_t)length, flow);
    free(data);
    fclose(file);
    return result;
}

/* All samples and all input flows stay resident until exit. Effect changes
 * only stop/recycle the instance and replace the DSP program. */
static int load_inputs(void) {
    int result = afx_bank_load_file(&bank, "/pc/inputs.afb");
    if (result) result = afx_bank_load_file(&bank, ENJ_CBASEPATH "inputs.afb");
    for (unsigned i = 0; !result && i < INPUT_COUNT; ++i) {
        char path[128];
        snprintf(path, sizeof(path), "/pc/%s.afx", inputs[i].stem);
        result = load_flow_file(path, &flows[i]);
        if (result) {
            snprintf(path, sizeof(path), ENJ_CBASEPATH "%s.afx", inputs[i].stem);
            result = load_flow_file(path, &flows[i]);
        }
    }
    if (!result) printf("DSP INPUTS: %u sample bytes, %u resident flows\n", (unsigned)bank.bytes, INPUT_COUNT);
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
        if (!result) running = false;
    }
    /* Stop the return, not the scene: replay reuses its program and delay RAM. */
    if (!result && installed_effect >= 0) result = afx_dsp_scene_returns(false);
    return result;
}

static int play(void) {
    afx_dsp_program_t program;
    unsigned source = selected_input();
    int result = unload();
    if (!result && installed_effect != selected) {
        result = afx_dsp_program_demo(&program, effects[selected].preset);
        if (!result) {
            /* A failed upload may have partially replaced the old scene. */
            installed_effect = -1;
            result = afx_dsp_scene_program(&program, sizeof(program));
            if (!result) installed_effect = selected;
        }
    }
    if (!result) result = afx_dsp_scene_returns(returns_enabled);
    if (!result) {
        result = afx_instance_activate(flows[source], &instance);
        if (!result) running = true;
    }
    if (!result) result = wait_for(instance, AFX_RUNNING);
    if (result) {
        unload();
        if (installed_effect < 0) afx_dsp_scene_disable();
        snprintf(message, sizeof(message), "Could not play effect (%d)", result);
        return result;
    }
    playing_effect = selected;
    snprintf(message, sizeof(message), "Playing %s with %s", effects[selected].name, inputs[source].name);
    printf("DSP EFFECT: %s / %s\n", effects[selected].preset, inputs[source].stem);
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
        char marker[4];
        snprintf(marker, sizeof(marker), "%c %c", index == selected ? '>' : ' ',
                 running && index == playing_effect ? '*' : ' ');
        enj_qfont_color_set(index == selected ? 255 : 210, index == selected ? 230 : 210,
                            index == selected ? 80 : 210);
        text((unsigned)row + 3, marker);
        /* Pixel positions keep columns aligned with the proportional font. */
        enj_qfont_write(effects[index].name, 34, (row + 3) * 16, PVR_LIST_PT_POLY);
        enj_qfont_write(effects[index].preset, 220, (row + 3) * 16, PVR_LIST_PT_POLY);
    }
    enj_qfont_color_set(230, 230, 230);
    char source[80];
    snprintf(source, sizeof(source), "INPUT: %s%s", input < 0 ? "Effect-tuned / " : "",
             inputs[selected_input()].name);
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
    if (!result) result = load_inputs();
    if (result || !enj_mode_push(&mode)) {
        printf("DSP player startup failed (%d)\n", result);
        afx_shutdown();
        return 1;
    }
    enj_state_run();
    unload();
    afx_dsp_scene_disable();
    for (unsigned i = 0; i < INPUT_COUNT; ++i)
        if (flows[i]) afx_asset_free(flows[i]);
    if (bank.asset) afx_bank_release(&bank);
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
        if (pad->button.LEFT == ENJ_BUTTON_DOWN_THIS_FRAME) input = input == -1 ? INPUT_COUNT - 1 : input - 1;
        if (pad->button.RIGHT == ENJ_BUTTON_DOWN_THIS_FRAME) input = input == INPUT_COUNT - 1 ? -1 : input + 1;
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
