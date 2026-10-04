#include <kos.h>

#include <aicaflow/bank.h>
#include <aicaflow/host.h>
#include <aicaflow/codec.h>
#include <enDjinn/enj_draw.h>
#include <enDjinn/enj_ctrl.h>
#include <enDjinn/enj_defs.h>
#include <enDjinn/enj_mode.h>
#include <enDjinn/enj_qfont.h>
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_state.h>

#include <stdio.h>
#include <stdalign.h>
#include <string.h>

alignas(32) static const unsigned char firmware[] = {
#embed "../../../firmware/aicaflow.drv"
};

/* PCM16 from dRxLaX's thrust_red.wav. The asset includes four copied guards. */
alignas(32) static const unsigned char engine_pcm16[] = {
#embed "../assets/drxlax_thrust_red_loop.pcm"
};

/* PCM16 from dRxLaX's mine_slide.wav, with four copied guards. */
alignas(32) static const unsigned char slide_pcm16[] = {
#embed "../assets/drxlax_mine_slide.pcm"
};

static int wait_for(afx_instance_t instance, uint32_t wanted, uint32_t timeout_ms) {
    uint64_t deadline = timer_ms_gettime64() + timeout_ms;
    do {
        afx_instance_status_t status;
        if (afx_update() < 0 || afx_instance_status(instance, &status)) return -1;
        if (status.state == wanted) return 0;
        if (status.state == AFX_DONE || status.state == AFX_ERROR) return -1;
        thd_sleep(1);
    } while (timer_ms_gettime64() < deadline);
    return -1;
}

enum {
    SLIDE_PAN_STEPS = 12,
    SLIDE_GUARD_FRAMES = 4,
    SLIDE_RATE_HZ = 44100,
    SLIDE_FRAMES = sizeof(slide_pcm16) / 2u - SLIDE_GUARD_FRAMES,
    SLIDE_DURATION_TICKS = (SLIDE_FRAMES * 1000u + SLIDE_RATE_HZ / 2u) / SLIDE_RATE_HZ,
    SLIDE_PAUSE_TICKS = 300,
    SLIDE_STREAM_BYTES = 8 + SLIDE_PAN_STEPS * (3 + 8) + 2 + 1
};

enum {
    ENGINE_BANK_OFFSET = 0,
    SLIDE_BANK_OFFSET = (sizeof(engine_pcm16) + 31u) & ~31u,
    DYNAMIC_BANK_BYTES = SLIDE_BANK_OFFSET + sizeof(slide_pcm16),
    DYNAMIC_BANK_ID_LOW = 0x44594e53u /* "SNYD"; fixed local demo bank identity. */
};

alignas(32) static uint8_t dynamic_bank_file[AFX_BANK_HEADER_BYTES + DYNAMIC_BANK_BYTES];

static_assert(SLIDE_FRAMES <= UINT16_MAX, "slide must fit in SCSP loop end");

enum { ENGINE_PAN_MIN = 5, ENGINE_PAN_SPAN = 21 };
static_assert(ENGINE_PAN_MIN + ENGINE_PAN_SPAN < 31,
              "engine pan must remain inside the stereo field");

/* UI: left 0..31 right. AICA: bit 4 selects left, low bits attenuate
 * the opposite side. Both 0x10 and 0x00 are centre. */
static uint8_t direct_pan(uint8_t pan) {
    return pan < 16 ? (uint8_t)(0x10u | (15u - pan)) : pan - 16u;
}

static uint8_t slide_pan(uint32_t step, bool left_to_right) {
    uint8_t pan = (uint8_t)((SLIDE_PAN_STEPS - step) * 31u / SLIDE_PAN_STEPS);
    return left_to_right ? 31u - pan : pan;
}

static void configure_setup(uint16_t fields[AFX_FIELD_COUNT], uint32_t bank_offset,
                            uint16_t loop_end, bool loop, uint8_t pan) {
    memset(fields, 0, AFX_FIELD_COUNT * sizeof(*fields));
    fields[AFX_FIELD_CONTROL] = (uint16_t)((loop ? 0x0200u : 0) | (bank_offset >> 16));
    fields[AFX_FIELD_SAMPLE_LOW] = (uint16_t)bank_offset;
    if (loop_end) fields[AFX_FIELD_LOOP_END] = loop_end;
    fields[AFX_FIELD_ENV_AD] = 0x001f;
    fields[AFX_FIELD_ENV_DR] = 0x001f;
    fields[AFX_FIELD_DIRECT] = (uint16_t)(0x0f00u | direct_pan(pan));
    fields[AFX_FIELD_MIX] = 0x0024;     /* No attenuation, open filter. */
    for (unsigned field = AFX_FIELD_FILTER_LEVEL0; field <= AFX_FIELD_FILTER_LEVEL4; ++field)
        fields[field] = 0x1fff;
    /* Let the filter envelope follow new cutoff targets while the loop plays. */
    fields[AFX_FIELD_FILTER_AD] = fields[AFX_FIELD_FILTER_DR] = 0x1f1f;
}

static int make_bank_flow(const afx_bank_t *bank, const uint16_t fields[AFX_FIELD_COUNT],
                          uint32_t bank_offset, uint32_t sample_bytes, const uint8_t *stream,
                          uint32_t stream_bytes, uint32_t flags, afx_asset_t *out_flow) {
    uint8_t file[96 + AFX_SETUP_BYTES + SLIDE_STREAM_BYTES] = {0};
    uint32_t image_bytes = AFX_SETUP_BYTES + stream_bytes;
    if (!bank || !bank->asset || !out_flow || stream_bytes > SLIDE_STREAM_BYTES) return -AFX_BAD_BOUNDS;
    afx_write32(file, AFX_FILE_MAGIC); afx_write32(file + 4, AFX_FILE_VERSION);
    afx_write32(file + 8, 96 + image_bytes); afx_write32(file + 12, flags);
    afx_write32(file + 16, 96); afx_write32(file + 20, image_bytes);
    afx_write32(file + 24, AFX_SETUP_BYTES); afx_write32(file + 28, stream_bytes);
    afx_write32(file + 36, 1); afx_write32(file + 40, bank->id.low); afx_write32(file + 44, bank->id.high);
    afx_write32(file + 48, 80); afx_write32(file + 52, 1);
    afx_write32(file + 64, 1); afx_write32(file + 68, 1000); afx_write32(file + 72, 1);
    afx_write32(file + 80, 0); afx_write32(file + 84, bank_offset); afx_write32(file + 88, sample_bytes);
    for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field)
        afx_write16(file + 96 + field * 2u, fields[field]);
    memcpy(file + 96 + AFX_SETUP_BYTES, stream, stream_bytes);
    afx_write32(file + 32, afx_control_id(file + 96, image_bytes));
    return afx_bank_flow_upload(bank, file, 96 + image_bytes, out_flow);
}

static int load_dynamic_bank(afx_bank_t *bank) {
    uint8_t *file = dynamic_bank_file;
    afx_write32(file, AFX_BANK_MAGIC); afx_write32(file + 4, AFX_BANK_VERSION);
    afx_write32(file + 8, DYNAMIC_BANK_ID_LOW); afx_write32(file + 12, 0);
    afx_write32(file + 16, AFX_BANK_HEADER_BYTES); afx_write32(file + 20, DYNAMIC_BANK_BYTES);
    afx_write32(file + 24, sizeof(dynamic_bank_file));
    memcpy(file + AFX_BANK_HEADER_BYTES + ENGINE_BANK_OFFSET, engine_pcm16, sizeof(engine_pcm16));
    memcpy(file + AFX_BANK_HEADER_BYTES + SLIDE_BANK_OFFSET, slide_pcm16, sizeof(slide_pcm16));
    return afx_bank_load_memory(bank, file, sizeof(dynamic_bank_file));
}

static int append_event(uint8_t *stream, uint32_t capacity, uint32_t *written,
                        const afx_event_t *event, const uint16_t *values) {
    uint32_t bytes;
    if (*written > capacity ||
        afx_encode_event(stream + *written, capacity - *written, event, values, &bytes)) return -1;
    *written += bytes;
    return 0;
}

static int make_engine_loop(const afx_bank_t *bank, afx_asset_t *out_flow) {
    uint16_t fields[AFX_FIELD_COUNT];
    uint8_t stream[9];
    uint16_t note[] = {0x0000, 0xff24}; /* Muted until the first live engine patch. */
    uint32_t written = 0;
    afx_event_t event = {.opcode = AFX_OP_NOTE_PL, .channel = 0, .setup = 0,
                         .mask = AFX_NOTE_PL_MASK};

    configure_setup(fields, ENGINE_BANK_OFFSET, (uint16_t)(sizeof(engine_pcm16) / 2u - 5u), true, 0);
    if (append_event(stream, sizeof(stream), &written, &event, note)) return -1;
    stream[written++] = AFX_OP_PARK;

    return make_bank_flow(bank, fields, ENGINE_BANK_OFFSET, sizeof(engine_pcm16), stream, written,
                          AFX_FLAG_CONTROLLED, out_flow);
}

/* The slide is complete AFX automation: once activated, SH-4 sends it no patches. */
static int make_slide_sequence(const afx_bank_t *bank, bool left_to_right, afx_asset_t *out_flow) {
    uint16_t fields[AFX_FIELD_COUNT];
    uint8_t stream[SLIDE_STREAM_BYTES];
    uint16_t note[] = {0x0000, 0x0024};
    uint32_t written = 0, previous_tick = 0;
    afx_event_t event = {.opcode = AFX_OP_NOTE_PL, .channel = 0, .setup = 0,
                         .mask = AFX_NOTE_PL_MASK};

    configure_setup(fields, SLIDE_BANK_OFFSET, SLIDE_FRAMES - 1u, false, slide_pan(0, left_to_right));
    if (append_event(stream, sizeof(stream), &written, &event, note)) return -1;
    for (uint32_t step = 1; step <= SLIDE_PAN_STEPS; ++step) {
        uint32_t tick = SLIDE_DURATION_TICKS * step / SLIDE_PAN_STEPS;
        uint16_t direct = (uint16_t)(0x0f00u | direct_pan(slide_pan(step, left_to_right)));
        event = (afx_event_t){.opcode = AFX_OP_WAIT16, .wait = tick - previous_tick};
        if (append_event(stream, sizeof(stream), &written, &event, NULL)) return -1;
        event = (afx_event_t){.opcode = AFX_OP_PATCH, .channel = 0,
                              .mask = 1u << AFX_FIELD_DIRECT};
        if (append_event(stream, sizeof(stream), &written, &event, &direct)) return -1;
        previous_tick = tick;
    }
    event = (afx_event_t){.opcode = AFX_OP_KEYOFF, .channel = 0};
    if (append_event(stream, sizeof(stream), &written, &event, NULL) ||
        written >= sizeof(stream)) return -1;
    stream[written++] = AFX_OP_END;

    return make_bank_flow(bank, fields, SLIDE_BANK_OFFSET, sizeof(slide_pcm16), stream, written, 0, out_flow);
}

/* Call this from the game loop for an engine instance. Pan is 0..31, while
 * intensity drives TL attenuation. Pitch is the native octave/FNS word. */
static int update_engine(afx_instance_t instance, uint8_t pan, uint8_t intensity,
                         uint16_t pitch, uint8_t filter_q, uint8_t brightness,
                         uint16_t lfo) {
    /* Brightness 0..15 maps to native 13-bit cutoff words, not MIX bits.
     * All envelope targets agree; the sustain stage follows live changes. */
    uint16_t cutoff = (uint16_t)(0x1500u + (uint32_t)brightness * 0x0900u / 15u);
    uint16_t values[] = {
        pitch,
        lfo,
        (uint16_t)(0x0f00u | direct_pan(pan)),
        /* TL includes 24 steps of headroom; Q is MIX bits 0..4.
         * LPOFF and VOFF stay clear: filter and attenuation remain enabled. */
        (uint16_t)((24u + (uint32_t)(255u - intensity) * 20u / 255u) << 8 | filter_q),
        cutoff, cutoff, cutoff, cutoff, cutoff
    };
    uint32_t mask = (1u << AFX_FIELD_PITCH) | (1u << AFX_FIELD_LFO) |
                    (1u << AFX_FIELD_DIRECT) | (1u << AFX_FIELD_MIX);
    for (unsigned field = AFX_FIELD_FILTER_LEVEL0; field <= AFX_FIELD_FILTER_LEVEL4; ++field)
        mask |= 1u << field;
    return afx_instance_patch(instance, 0, mask, values);
}

static int mute_engine(afx_instance_t instance, uint8_t filter_q) {
    uint16_t mix = (uint16_t)(0xff00u | filter_q);
    return afx_instance_patch(instance, 0, 1u << AFX_FIELD_MIX, &mix);
}

static uint8_t triangle(uint32_t phase) {
    phase %= 510;
    return phase <= 255 ? phase : 510 - phase;
}

static uint16_t engine_lfo(uint8_t motion) {
    uint16_t pitch_depth = 1u + (uint32_t)motion * 2u / 255u;
    uint16_t amplitude_depth = 1u + (uint32_t)motion / 128u;
    return (uint16_t)(6u << 10 | 2u << 8 | pitch_depth << 5 | 2u << 3 |
                      amplitude_depth);
}

static uint8_t clamp_u8(int value) {
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

typedef struct {
    int result;
    uint32_t frame, slide_started, next_engine_update, next_slide;
    afx_asset_t slide_flows[2];
    afx_instance_t engine, slide;
    uint16_t pitch, lfo;
    uint8_t engine_pan, throttle, filter_q, brightness, slide_pan;
    bool engine_enabled, engine_muted, slide_enabled, slide_active, slide_recycling,
         slide_stopping, slide_left_to_right, next_slide_left_to_right;
} demo_state_t;

static demo_state_t demo;

static void update_engine_parameters(demo_state_t *state,
                                     const enj_ctrlr_state_t *controller) {
    if (controller) {
        uint8_t proximity = (uint8_t)(127 - controller->joyy); /* Stick up is close. */
        uint8_t gas = controller->rtrigger, brake = controller->ltrigger;
        uint8_t rpm = clamp_u8(32 + (int)gas * 223 / 255 - (int)brake * 96 / 255);
        state->engine_pan = ENGINE_PAN_MIN +
                            (uint32_t)(controller->joyx + 128) * ENGINE_PAN_SPAN / 255u;
        state->throttle = clamp_u8(144 + (int)proximity * 47 / 255 +
                                   (int)gas * 96 / 255 - (int)brake * 64 / 255);
        state->pitch = (uint16_t)(0x0040u + (uint32_t)rpm * 0x03bfu / 255u);
        state->filter_q = 6u + (uint32_t)gas * 8u / 255u;
        state->brightness = clamp_u8(4 + (int)proximity * 5 / 255 +
                                     (int)gas * 6 / 255 - (int)brake * 3 / 255);
        state->lfo = engine_lfo(255u - rpm); /* Idle shakes more than high RPM. */
        return;
    }
    uint8_t pan_motion = triangle(state->frame * 4u);
    uint8_t intensity_motion = triangle(state->frame * 2u + 170u);
    uint8_t lfo_motion = triangle(state->frame * 3u + 85u);
    state->engine_pan = ENGINE_PAN_MIN + (uint32_t)pan_motion * ENGINE_PAN_SPAN / 255u;
    state->throttle = 144u + (uint32_t)intensity_motion * 111u / 255u;
    state->pitch = (uint16_t)(0x0080u + (uint32_t)state->throttle * 0x037fu / 255u);
    state->filter_q = 6u + (uint32_t)triangle(state->frame * 3u) * 8u / 255u;
    state->brightness = 4u + (uint32_t)triangle(state->frame * 5u) * 11u / 255u;
    state->lfo = engine_lfo(lfo_motion);
}

static void draw_sprite(float x, float y, float width, float height, uint32_t color) {
    pvr_sprite_cxt_t context;
    pvr_sprite_hdr_t header;
    float corners[4][3] = {
        {x * ENJ_XSCALE, (y + height), 1.0f},
        {x * ENJ_XSCALE, y, 1.0f},
        {(x + width) * ENJ_XSCALE, y, 1.0f},
        {(x + width) * ENJ_XSCALE, (y + height), 1.0f},
    };
    pvr_sprite_cxt_col(&context, PVR_LIST_OP_POLY);
    pvr_sprite_compile(&header, &context);
    header.argb = color;
    enj_draw_sprite(corners, &header, NULL);
}

static float pan_x(uint8_t pan) { return 64.0f + (float)pan * 16.5f; }

static void render_sprites(void *data) {
    demo_state_t *state = data;
    const uint32_t rail = 0xff394250, engine = 0xff20b8ff, slide = 0xffffa52c;
    draw_sprite(64, 170, 512, 4, rail);
    draw_sprite(64, 300, 512, 4, rail);
    draw_sprite(pan_x(state->engine_pan) - 11, 158, 22, 28,
                state->engine_enabled ? engine : 0xff36536a);
    draw_sprite(64, 220, 512, 12, 0xff202a36);
    draw_sprite(64, 220, (float)state->throttle * 2.0f, 12, engine);
    draw_sprite(64, 250, 512, 12, 0xff202a36);
    draw_sprite(64, 250, (float)state->pitch * 0.5f, 12, 0xff7de6ff);
    draw_sprite(pan_x(state->slide_pan) - 9, 288, 18, 28,
                state->slide_active ? slide : 0xff6b5740);
}

static void render_text(void *data) {
    demo_state_t *state = data;
    char line[64];
    enj_qfont_color_set(220, 235, 255);
    enj_qfont_write("AICAFLOW DYNAMIC SFX", 20, 18, PVR_LIST_PT_POLY);
    enj_qfont_color_set(100, 180, 255);
    enj_qfont_write(state->engine_enabled ? "ENGINE: ANALOG SH-4 PATCHES [A: SELECTED]"
                                          : "ENGINE: ANALOG SH-4 PATCHES [A: SELECT]",
                    20, 82, PVR_LIST_PT_POLY);
    enj_qfont_write("L                         PAN                         R", 42, 190,
                    PVR_LIST_PT_POLY);
    snprintf(line, sizeof(line), "PAN %2u/31  INT %3u  PITCH %04x  Q %2u  LPF %2u  LFO %04x",
             state->engine_pan, state->throttle, state->pitch, state->filter_q,
             state->brightness, state->lfo);
    enj_qfont_write(line, 64, 238, PVR_LIST_PT_POLY);
    enj_qfont_color_set(255, 180, 60);
    enj_qfont_write(state->slide_enabled ? "SLIDE: AUTHORED AFX SEQUENCE [B: SELECTED]"
                                        : "SLIDE: AUTHORED AFX SEQUENCE [B: SELECT]",
                    20, 270, PVR_LIST_PT_POLY);
    snprintf(line, sizeof(line), "PAN %2u/31   %s   12 PACKED PATCHES", state->slide_pan,
             state->slide_active ? (state->slide_left_to_right ? "LEFT -> RIGHT" : "RIGHT -> LEFT")
                                : "PAUSED");
    enj_qfont_write(line, 64, 328, PVR_LIST_PT_POLY);
    enj_qfont_color_set(190, 190, 190);
    enj_qfont_write("Blue: thrust_red.wav   Orange: mine_slide.wav", 20, 398,
                    PVR_LIST_PT_POLY);
    enj_qfont_write("Stick: pan/distance   R: gas   L: brake   START: exit", 20, 422,
                    PVR_LIST_PT_POLY);
    if (state->result) {
        snprintf(line, sizeof(line), "AICAFLOW ERROR %d", state->result);
        enj_qfont_color_set(255, 70, 70);
        enj_qfont_write(line, 20, 450, PVR_LIST_PT_POLY);
    }
}

static int update_slide(demo_state_t *state, uint32_t now) {
    if (state->slide) {
        afx_instance_status_t status;
        int result = afx_instance_status(state->slide, &status);
        if (result == -AFX_STALE_GENERATION && state->slide_recycling) {
            state->slide = 0;
            state->slide_recycling = false;
            state->slide_stopping = false;
            state->next_slide = now + SLIDE_PAUSE_TICKS;
        } else if (result) {
            return result;
        } else if (status.state == AFX_ERROR) {
            return status.result ? -(int)status.result : -AFX_BAD_COMMAND;
        } else if (status.state == AFX_DONE && !state->slide_recycling) {
            result = afx_instance_recycle(state->slide);
            if (!result) state->slide_recycling = true;
            else if (result != -AFX_BUSY && result != -AFX_IPC_FULL) return result;
        } else if (!state->slide_enabled && !state->slide_stopping) {
            result = afx_instance_stop(state->slide);
            if (!result) state->slide_stopping = true;
            else if (result != -AFX_BUSY && result != -AFX_IPC_FULL) return result;
        }
    }
    if (state->slide_enabled && !state->slide && (int32_t)(now - state->next_slide) >= 0) {
        int result = afx_instance_activate(state->slide_flows[state->next_slide_left_to_right],
                                           &state->slide);
        if (!result) {
            state->slide_left_to_right = state->next_slide_left_to_right;
            state->next_slide_left_to_right = !state->next_slide_left_to_right;
            state->slide_started = afx_instance_start_tick(state->slide);
        } else if (result != -AFX_BUSY && result != -AFX_IPC_FULL) {
            return result;
        }
    }
    return 0;
}

static void update(void *data) {
    demo_state_t *state = data;
    uint32_t now = afx_status_timer_ticks();
    enj_ctrlr_state_t **controllers = enj_ctrl_get_states();
    enj_ctrlr_state_t *controller = NULL;
    for (size_t i = 0; i < enj_ctrl_states_length() && !controller; ++i)
        controller = controllers[i];
    if (controller && controller->button.START == ENJ_BUTTON_DOWN_THIS_FRAME) {
        enj_state_flag_shutdown(NULL);
        return;
    }
    if (controller && controller->button.A == ENJ_BUTTON_DOWN_THIS_FRAME) {
        state->engine_enabled = true;
        state->slide_enabled = false;
    }
    if (controller && controller->button.B == ENJ_BUTTON_DOWN_THIS_FRAME) {
        state->engine_enabled = false;
        state->slide_enabled = true;
        state->next_slide = now;
    }
    if (!state->result && state->engine_enabled && !state->slide &&
        (int32_t)(now - state->next_engine_update) >= 0) {
        update_engine_parameters(state, controller);
        int result = update_engine(state->engine, state->engine_pan, state->throttle,
                                   state->pitch, state->filter_q, state->brightness,
                                   state->lfo);
        if (!result) state->engine_muted = false;
        else if (result != -AFX_BUSY && result != -AFX_IPC_FULL) state->result = result;
        state->next_engine_update = now + 16;
        ++state->frame;
    }
    if (!state->result && !state->engine_enabled && !state->engine_muted) {
        int result = mute_engine(state->engine, state->filter_q);
        if (!result) state->engine_muted = true;
        else if (result != -AFX_BUSY && result != -AFX_IPC_FULL) state->result = result;
    }
    if (!state->result && afx_update() < 0) state->result = -1;
    if (!state->result) state->result = update_slide(state, now);
    uint32_t elapsed = (int32_t)(now - state->slide_started) > 0 ?
                       now - state->slide_started : 0;
    uint32_t step = elapsed * SLIDE_PAN_STEPS / SLIDE_DURATION_TICKS;
    state->slide_active = state->slide && !state->slide_recycling && elapsed < SLIDE_DURATION_TICKS;
    state->slide_pan = state->slide_active ? slide_pan(step > SLIDE_PAN_STEPS ? SLIDE_PAN_STEPS : step,
                                                      state->slide_left_to_right) : 0;
    enj_render_list_add(PVR_LIST_OP_POLY, render_sprites, state);
    enj_render_list_add(PVR_LIST_PT_POLY, render_text, state);
    if (state->result) enj_state_flag_shutdown(NULL);
}

static enj_mode_t mode = {.mode_updater = update, .data = &demo, .name = "Dynamic SFX"};

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    afx_asset_t engine_loop = 0;
    afx_bank_t bank = {0};
    afx_instance_t engine = 0;
    enj_state_init_defaults();
    if (enj_state_startup()) return 1;
    int result = afx_init(firmware, sizeof(firmware));
    if (!result) result = load_dynamic_bank(&bank);
    if (!result) result = make_engine_loop(&bank, &engine_loop);
    if (!result) result = make_slide_sequence(&bank, false, &demo.slide_flows[0]);
    if (!result) result = make_slide_sequence(&bank, true, &demo.slide_flows[1]);
    if (!result) result = afx_instance_activate(engine_loop, &engine);
    if (!result) result = wait_for(engine, AFX_PARKED, 1000);
    demo.engine = engine;
    demo.engine_enabled = true;
    demo.slide_enabled = false;
    demo.next_engine_update = afx_status_timer_ticks();
    demo.next_slide = demo.next_engine_update;
    if (!result && !enj_mode_push(&mode)) result = -1;
    if (!result) enj_state_run();
    if (!result) result = demo.result;

    afx_shutdown();
    printf("Aicaflow dynamic SFX: %s (%d)\n", result ? "FAIL" : "PASS", result);
    return result ? 1 : 0;
}
