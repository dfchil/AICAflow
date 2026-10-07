#include <kos.h>
#include <kos/net.h>
#include <dc/g2bus.h>
#include <dc/sound/sound.h>
#include <aicaflow/dsp.h>
#include <aicaflow/host.h>
#include <aicaflow/codec.h>
#include <aicaflow/bank.h>
#include <enDjinn/enj_qfont.h>
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_mode.h>
#include <enDjinn/enj_state.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "terminal.h"

#define TUNER_PORT 31337u
#define TUNER_MAGIC 0x31544641u /* "AFT1", little-endian. */
#define TUNER_VERSION 1u
#define TUNER_HEADER_BYTES 16u
/* The host-side AFX file includes checkpoints in addition to its AICA image. */
#define TUNER_MAX_UPLOAD 0x400000u
enum { TUNER_PING = 1, TUNER_UPLOAD, TUNER_PLAY, TUNER_STOP, TUNER_PATCH,
       TUNER_INSTANCE_GAIN, TUNER_STATUS, TUNER_EXIT, TUNER_UPLOAD_PLAY,
       TUNER_UPLOAD_BEGIN, TUNER_UPLOAD_CHUNK, TUNER_UPLOAD_COMMIT,
       TUNER_DSP_ENABLE, TUNER_DSP_DISABLE, TUNER_ASSET_READ, TUNER_PLAY_REGION, TUNER_MESSAGE,
       TUNER_DSP_PROGRAM, TUNER_REGISTER_READ, TUNER_BANK_COMMIT,
       TUNER_CONTROL_COMMIT_PLAY, TUNER_DSP_ROOM, TUNER_LANE_MUTE, TUNER_DSP_RETURNS,
       TUNER_DSP_PROGRAM_RING, TUNER_SEEK_INDEX_COMMIT_PLAY, TUNER_RESET };
#define DSP_PROGRAM_BYTES AFX_DSP_PROGRAM_BYTES
#define TUNER_MAX_ASSET_READ 4096u


static const uint8_t firmware[] = {
#embed "../../../firmware/aicaflow.drv"
};
static terminal_buffer_t terminal;
static int exiting;
static uint8_t upload_progress_line;

typedef struct {
    afx_asset_t asset;
    afx_instance_t instance;
    afx_bank_t bank;
    uint8_t *upload;
    uint32_t upload_total;
    uint32_t upload_received;
    uint32_t playback_ticks;
    uint32_t playback_tick_rate_num;
    uint32_t playback_tick_rate_den;
    uint32_t playback_started;
    uint32_t playback_next_update;
    uint32_t region_end;
    uint8_t region_active;
    uint8_t music_gain;
    uint8_t lane_count;
    uint8_t playback_progress_line;
    uint8_t playback_active;
} tuner_state_t;

static void console_mode_update(void *unused) {
    (void)unused;
    enj_render_list_add(PVR_LIST_PT_POLY, terminal_render, &terminal);
}
static enj_mode_t console_mode = { .mode_updater = console_mode_update, .name = "Aicaflow tuner" };
static void present(void) { enj_render_next_frame(&console_mode); }
static int console_init(void) {
    enj_state_init_defaults();
    if (enj_state_startup()) return -1;
    /* enDjinn starts KOS's sound interface for ordinary applications. The
     * tuner replaces the ARM program, so release KOS's AICA ownership before
     * afx_init installs the AICAflow firmware. */
    snd_shutdown();
    enj_qfont_color_set(255, 255, 255);
    terminal_clear(&terminal);
    terminal_writeline_screen(&terminal, "Aicaflow persistent tuner");
    present();
    return 0;
}
static void trace(const char *format, ...) {
    char line[160];
    va_list args;
    va_start(args, format); vsnprintf(line, sizeof(line), format, args); va_end(args);
    terminal_writeline_screen(&terminal, line); present();
}
static void upload_progress(const tuner_state_t *state) {
    char line[TERMINAL_WIDTH + 1];
    unsigned percent = state->upload_total ? state->upload_received * 100u / state->upload_total : 0;
    unsigned filled = percent * 20u / 100u;
    snprintf(line, sizeof(line), "UPLOAD [%.*s%.*s] %3u%% %lu/%lu KiB",
             (int)filled, "####################", (int)(20u - filled), "....................", percent,
             (unsigned long)(state->upload_received / 1024u), (unsigned long)((state->upload_total + 1023u) / 1024u));
    terminal_setline(&terminal, upload_progress_line, line); present();
}
static int bank_flow_properties(const uint8_t *file, uint32_t bytes, tuner_state_t *state) {
    if (!file || bytes < AFX_FILE_HEADER_BYTES || afx_read32(file) != AFX_FILE_MAGIC ||
        afx_read32(file + 4) != AFX_FILE_VERSION || afx_read32(file + 8) != bytes)
        return -AFX_BAD_FORMAT;
    uint32_t image = afx_read32(file + 16), image_size = afx_read32(file + 20);
    uint32_t setups = afx_read32(file + 36), stream = afx_read32(file + 24), channels = afx_read32(file + 64);
    uint32_t flags = afx_read32(file + 12);
    if (image > bytes || image_size > bytes - image || stream < setups * AFX_SETUP_BYTES ||
        channels > AFX_MAX_FLOW_CHANNELS) return -AFX_BAD_FORMAT;
    state->lane_count = 0;
    if (flags & AFX_FLAG_LANES) {
        const uint8_t *lanes = file + image + setups * AFX_SETUP_BYTES;
        if (channels > stream - setups * AFX_SETUP_BYTES) return -AFX_BAD_FORMAT;
        for (uint32_t i = 0; i < channels; ++i)
            if (lanes[i] + 1u > state->lane_count) state->lane_count = lanes[i] + 1u;
    }
    uint64_t ticks;
    afx_result_t result = afx_flow_duration(file, bytes, &ticks,
                                          &state->playback_tick_rate_num,
                                          &state->playback_tick_rate_den);
    if (result) return -(int)result;
    state->playback_ticks = ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
    return AFX_OK;
}
static uint32_t playback_milliseconds(const tuner_state_t *state, uint32_t ticks) {
    return (uint32_t)(((uint64_t)ticks * 1000u * state->playback_tick_rate_den) /
                      state->playback_tick_rate_num);
}
static void playback_progress(const tuner_state_t *state, uint32_t elapsed) {
    char line[TERMINAL_WIDTH + 1];
    uint32_t total = state->playback_ticks;
    uint32_t percent = total ? (uint32_t)(((uint64_t)elapsed * 100u) / total) : 0;
    uint32_t filled, elapsed_ms, total_ms;
    if (percent > 100u) percent = 100u;
    filled = percent * 20u / 100u;
    elapsed_ms = playback_milliseconds(state, elapsed > total ? total : elapsed);
    total_ms = playback_milliseconds(state, total);
    snprintf(line, sizeof(line), "PLAY   [%.*s%.*s] %3lu%% %02lu:%02lu/%02lu:%02lu",
             (int)filled, "####################", (int)(20u - filled), "....................",
             (unsigned long)percent, (unsigned long)(elapsed_ms / 60000u),
             (unsigned long)((elapsed_ms / 1000u) % 60u), (unsigned long)(total_ms / 60000u),
             (unsigned long)((total_ms / 1000u) % 60u));
    terminal_setline(&terminal, state->playback_progress_line, line); present();
}
static void playback_begin(tuner_state_t *state) {
    state->region_active = 0;
    state->playback_started = afx_status_timer_ticks();
    state->playback_next_update = state->playback_started;
    state->playback_active = state->playback_ticks != 0;
    if (!state->playback_active) return;
    state->playback_progress_line = terminal.cur_line;
    terminal_writeline_screen(&terminal, "");
    playback_progress(state, 0);
}
static void playback_update(tuner_state_t *state) {
    afx_instance_status_t status;
    uint32_t now, elapsed;
    if (!state->playback_active || !state->instance ||
        afx_instance_status(state->instance, &status) != AFX_OK) return;
    now = afx_status_timer_ticks();
    elapsed = now - state->playback_started;
    if (state->region_active && status.state == AFX_RUNNING &&
        (int32_t)(now - state->region_end) >= 0) {
        if (afx_instance_stop(state->instance) == AFX_OK) {
            state->region_active = state->playback_active = 0;
        }
        return;
    }
    if (status.state != AFX_RUNNING) {
        if (status.state == AFX_DONE) playback_progress(state, state->playback_ticks);
        state->playback_active = 0;
    } else if ((int32_t)(now - state->playback_next_update) >= 0) {
        playback_progress(state, elapsed);
        state->playback_next_update = now + 250u;
    }
}
static uint16_t read16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write16(uint8_t *p, uint16_t value) { p[0] = value; p[1] = value >> 8; }
static void write32(uint8_t *p, uint32_t value) {
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}
static int io_all(int fd, void *buffer, uint32_t bytes, int receive) {
    uint8_t *cursor = buffer;
    while (bytes) {
        struct pollfd pollfd = { .fd = fd, .events = receive ? POLLIN : POLLOUT };
        int ready = poll(&pollfd, 1, 1000);
        (void)afx_update();
        /* KOS may report POLLHUP together with buffered POLLIN data. Consume
         * the requested event first; recv/send provides the definitive EOF. */
        if (ready != 1 || (pollfd.revents & (POLLERR | POLLNVAL)) ||
            !(pollfd.revents & (receive ? POLLIN : POLLOUT))) return -1;
        int count = receive ? recv(fd, cursor, bytes, 0) : send(fd, cursor, bytes, 0);
        if (count <= 0) return -1;
        cursor += count; bytes -= (uint32_t)count;
    }
    return 0;
}
/* Once a complete, valid header has arrived, KOS receives a fragmented body
 * most reliably with blocking recv. Never use this for the initial header:
 * an empty host connection may otherwise wedge the single-client server. */
static int recv_payload(int fd, void *buffer, uint32_t bytes) {
    uint8_t *cursor = buffer;
    while (bytes) {
        int count = recv(fd, cursor, bytes, 0);
        if (count <= 0) return -1;
        cursor += count;
        bytes -= (uint32_t)count;
    }
    return 0;
}
static int reply(int fd, uint16_t opcode, uint32_t sequence, int result,
                 const uint8_t *data, uint32_t bytes) {
    uint8_t header[TUNER_HEADER_BYTES], code[4];
    write32(header, TUNER_MAGIC); write16(header + 4, TUNER_VERSION);
    write16(header + 6, opcode | 0x8000u); write32(header + 8, sequence); write32(header + 12, bytes + 4u);
    write32(code, (uint32_t)result);
    return io_all(fd, header, sizeof(header), 0) || io_all(fd, code, sizeof(code), 0) ||
           (bytes && io_all(fd, (void *)data, bytes, 0));
}
static int stop_instance(tuner_state_t *state) {
    if (!state->instance) return AFX_OK;
    afx_instance_status_t status;
    if (afx_update() < 0 || afx_instance_status(state->instance, &status) != AFX_OK) return -AFX_TIMEOUT;
    int result = (status.state == AFX_DONE || status.state == AFX_ERROR) ?
        afx_instance_recycle(state->instance) : afx_instance_stop(state->instance);
    if (result) return result;
    int recycling = status.state == AFX_DONE || status.state == AFX_ERROR;
    for (unsigned ms = 0; ms < 1000; ++ms) {
        if (afx_update() < 0) return -AFX_TIMEOUT;
        int current = afx_instance_status(state->instance, &status);
        if (recycling && current == -AFX_STALE_GENERATION) {
            state->instance = 0;
            return AFX_OK;
        }
        if (!recycling && current == AFX_OK && (status.state == AFX_DONE || status.state == AFX_ERROR)) {
            result = afx_instance_recycle(state->instance);
            if (result) return result;
            recycling = 1;
        }
        thd_sleep(1);
    }
    return -AFX_TIMEOUT;
}
static int wait_state(afx_instance_t instance, uint32_t wanted) {
    for (unsigned ms = 0; ms < 1000; ++ms) {
        afx_instance_status_t status;
        if (afx_update() < 0) return -AFX_TIMEOUT;
        if (afx_instance_status(instance, &status) == AFX_OK) {
            if (status.state == wanted) return AFX_OK;
            if (status.state == AFX_DONE || status.state == AFX_ERROR) return -AFX_BAD_COMMAND;
        }
        thd_sleep(1);
    }
    return -AFX_TIMEOUT;
}
static int wait_running(afx_instance_t instance) { return wait_state(instance, AFX_RUNNING); }
/* Queue gain behind ACTIVATE so the first authored deadline uses the selected level. */
static int activate_instance(tuner_state_t *state) {
    int result = afx_instance_activate(state->asset, &state->instance);
    if (!result) result = afx_instance_gain(state->instance, state->music_gain);
    return result ? result : wait_running(state->instance);
}
static int dsp_default_room_upload(void) {
    afx_dsp_program_t program;
    int result = afx_dsp_program_room(&program, 22936, 11464, false, 256, false);
    return result ? result : afx_dsp_scene_program(&program, sizeof(program));
}
static int dsp_room_upload(const uint8_t *payload, uint32_t bytes) {
    afx_dsp_program_t program;
    if (bytes != 8 || payload[6] & ~3u || payload[7]) return -AFX_BAD_COMMAND;
    int result = afx_dsp_program_room(&program, (int16_t)read16(payload),
                                      (int16_t)read16(payload + 2),
                                      payload[6] & 1u, (int16_t)read16(payload + 4),
                                      payload[6] & 2u);
    return result ? result : afx_dsp_scene_program(&program, sizeof(program));
}
static int play_region(tuner_state_t *state, uint32_t start_ms, uint32_t duration_ms) {
    if (!state->asset || !duration_ms || duration_ms > 30000u ||
        start_ms >= playback_milliseconds(state, state->playback_ticks)) return -AFX_BAD_BOUNDS;
    uint32_t tick = (uint32_t)((uint64_t)start_ms * state->playback_tick_rate_num /
                               ((uint64_t)1000u * state->playback_tick_rate_den));
    int result = stop_instance(state);
    /* Silence activation until the existing checkpoint seek restores the passage. */
    if (!result) result = afx_instance_activate(state->asset, &state->instance);
    if (!result) result = wait_running(state->instance);
    if (!result) result = afx_instance_gain(state->instance, 0);
    if (!result) result = afx_instance_pause(state->instance);
    if (!result) result = wait_state(state->instance, AFX_PAUSED);
    int restored = afx_instance_gain(state->instance, state->music_gain);
    if (!result) result = restored;
    if (!result) result = afx_instance_seek_checkpoint(state->instance, tick, &tick);
    if (!result) result = wait_running(state->instance);
    if (result) { stop_instance(state); return result; }
    playback_begin(state);
    state->playback_started -= playback_milliseconds(state, tick);
    state->region_end = afx_status_timer_ticks() + duration_ms;
    state->region_active = 1;
    return AFX_OK;
}
static int replace_bank(tuner_state_t *state, const uint8_t *file, uint32_t bytes) {
    int result = stop_instance(state);
    if (!result && state->asset) result = afx_asset_free(state->asset);
    if (!result && state->bank.asset) result = afx_bank_release(&state->bank);
    state->asset = 0;
    state->lane_count = 0;
    state->playback_active = 0;
    if (!result) result = afx_bank_load_memory(&state->bank, file, bytes);
    return result;
}
static int replace_control_play(tuner_state_t *state, const uint8_t *file, uint32_t bytes) {
    int result = stop_instance(state);
    if (!result && state->asset) result = afx_asset_free(state->asset);
    state->asset = 0;
    state->lane_count = 0;
    state->playback_active = 0;
    if (!result) result = afx_bank_flow_upload(&state->bank, file, bytes, &state->asset);
    if (!result) result = activate_instance(state);
    if (!result) result = bank_flow_properties(file, bytes, state);
    if (!result) playback_begin(state);
    return result;
}
/* AFC is host-only. Stop the just-started control flow, attach its index, then
 * restart it so later region seeks have a validated checkpoint table. */
static int replace_seek_index_play(tuner_state_t *state, const uint8_t *file, uint32_t bytes) {
    int result = !state->asset ? -AFX_BAD_COMMAND : stop_instance(state);
    state->playback_active = 0;
    if (!result) result = afx_flow_seek_index_load_memory(state->asset, file, bytes);
    if (!result) result = activate_instance(state);
    if (!result) playback_begin(state);
    return result;
}
static void discard_upload(tuner_state_t *state) {
    free(state->upload);
    state->upload = NULL;
    state->upload_total = state->upload_received = 0;
}
/* A reset deliberately restarts the firmware instead of unwinding individual
 * assets. afx_init clears all AICA RAM, rebuilds the host allocator and starts
 * with no DSP program, bank, flow, instance or seek state. */
static int reset_tuner(tuner_state_t *state) {
    discard_upload(state);
    afx_shutdown();
    memset(state, 0, sizeof(*state));
    state->music_gain = 255;
    return afx_init(firmware, sizeof(firmware));
}
static int handle(int fd, tuner_state_t *state, const uint8_t *header, uint8_t *payload) {
    uint16_t opcode = read16(header + 6);
    uint32_t sequence = read32(header + 8), bytes = read32(header + 12);
    int result = AFX_OK;
    uint8_t status[28];
    if (opcode == TUNER_PING) {
        if (bytes) result = -AFX_BAD_COMMAND;
        else {
            write32(status, TUNER_RESET);
            return reply(fd, opcode, sequence, AFX_OK, status, 4);
        }
    } else if (opcode == TUNER_RESET) {
        result = bytes ? -AFX_BAD_COMMAND : reset_tuner(state);
        trace("TUNER reset result=%d", result);
    } else if (opcode == TUNER_UPLOAD_BEGIN) {
        uint32_t total = bytes == 4 ? read32(payload) : 0;
        if (!total || total > TUNER_MAX_UPLOAD) result = -AFX_BAD_BOUNDS;
        else {
            discard_upload(state);
            state->upload = malloc(total);
            if (!state->upload) result = -AFX_NO_HOST_RAM;
            else {
                state->upload_total = total;
                upload_progress_line = terminal.cur_line;
                upload_progress(state);
                terminal_writeline_screen(&terminal, "");
            }
        }
    } else if (opcode == TUNER_UPLOAD_CHUNK) {
        uint32_t offset = bytes >= 4 ? read32(payload) : UINT32_MAX;
        uint32_t data_bytes = bytes >= 4 ? bytes - 4 : 0;
        if (!state->upload || !data_bytes || offset > state->upload_total ||
            data_bytes > state->upload_total - offset || offset > state->upload_received)
            result = -AFX_BAD_BOUNDS;
        else if (offset < state->upload_received) {
            if (offset + data_bytes > state->upload_received ||
                memcmp(state->upload + offset, payload + 4, data_bytes)) result = -AFX_BAD_COMMAND;
        } else {
            memcpy(state->upload + offset, payload + 4, data_bytes);
            state->upload_received += data_bytes;
            upload_progress(state);
        }
    } else if (opcode == TUNER_BANK_COMMIT) {
        if (bytes || !state->upload || state->upload_received != state->upload_total)
            result = -AFX_BAD_COMMAND;
        else {
            result = replace_bank(state, state->upload, state->upload_total);
            trace("TUNER bank-commit bytes=%lu result=%d", (unsigned long)state->upload_total, result);
            discard_upload(state);
        }
    } else if (opcode == TUNER_CONTROL_COMMIT_PLAY) {
        if (bytes || !state->upload || state->upload_received != state->upload_total)
            result = -AFX_BAD_COMMAND;
        else {
            result = replace_control_play(state, state->upload, state->upload_total);
            trace("TUNER control-commit-play bytes=%lu result=%d", (unsigned long)state->upload_total, result);
            discard_upload(state);
        }
    } else if (opcode == TUNER_SEEK_INDEX_COMMIT_PLAY) {
        if (bytes || !state->upload || state->upload_received != state->upload_total)
            result = -AFX_BAD_COMMAND;
        else {
            result = replace_seek_index_play(state, state->upload, state->upload_total);
            trace("TUNER seek-index-commit-play bytes=%lu result=%d", (unsigned long)state->upload_total, result);
            discard_upload(state);
        }
    } else if (opcode == TUNER_PLAY) {
        result = !state->asset || bytes ? -AFX_BAD_COMMAND : stop_instance(state);
        if (!result) result = activate_instance(state);
        if (!result) playback_begin(state);
        trace("TUNER play result=%d", result);
    } else if (opcode == TUNER_MESSAGE) {
        if (!bytes || bytes > TERMINAL_WIDTH) result = -AFX_BAD_COMMAND;
        for (uint32_t i = 0; !result && i < bytes; ++i)
            if (payload[i] < 32u || payload[i] > 126u) result = -AFX_BAD_COMMAND;
        if (!result) trace("%.*s", (int)bytes, (const char *)payload);
    } else if (opcode == TUNER_PLAY_REGION) {
        result = bytes == 8 ? play_region(state, read32(payload), read32(payload + 4)) : -AFX_BAD_COMMAND;
        trace("TUNER region result=%d", result);
    } else if (opcode == TUNER_STOP) {
        result = bytes ? -AFX_BAD_COMMAND : stop_instance(state);
        trace("TUNER stop result=%d", result);
    } else if (opcode == TUNER_INSTANCE_GAIN) {
        result = bytes == 1 ? (state->instance ? afx_instance_gain(state->instance, payload[0]) : AFX_OK)
                            : -AFX_BAD_COMMAND;
        if (!result) state->music_gain = payload[0];
        trace("TUNER gain value=%u result=%d", bytes == 1 ? payload[0] : 0u, result);
    } else if (opcode == TUNER_DSP_ENABLE) {
        result = !bytes ? dsp_default_room_upload() : -AFX_BAD_COMMAND;
        trace("TUNER dsp-enable result=%d", result);
    } else if (opcode == TUNER_DSP_ROOM) {
        result = dsp_room_upload(payload, bytes);
        trace("TUNER dsp-room result=%d", result);
    } else if (opcode == TUNER_DSP_RETURNS) {
        result = bytes == 1 && payload[0] < 2 ? afx_dsp_scene_returns(payload[0]) : -AFX_BAD_COMMAND;
        trace("TUNER dsp-returns enabled=%u result=%d", bytes == 1 ? payload[0] : 2u, result);
    } else if (opcode == TUNER_LANE_MUTE) {
        uint32_t muted = bytes == 4 ? read32(payload) : UINT32_MAX;
        uint8_t values[32];
        uint32_t mask = state->lane_count >= 32 ? UINT32_MAX :
                        (state->lane_count ? (1u << state->lane_count) - 1u : 0u);
        if (!state->instance || bytes != 4 || !mask) result = -AFX_BAD_COMMAND;
        else {
            for (uint32_t i = 0; i < 32; ++i) values[i] = (muted >> i) & 1u;
            result = afx_instance_lanes_set(state->instance, AFX_LANE_MUTE, 0,
                                            mask, values);
        }
        trace("TUNER lane-mute result=%d", result);
    } else if (opcode == TUNER_DSP_PROGRAM) {
        result = afx_dsp_scene_program(payload,bytes);
        if (!result) {
            uint8_t data[DSP_PROGRAM_BYTES];
            for (uint32_t i = 0; i < 512; ++i) write16(data + i * 2, g2_read_32(0xa0703400u + i * 4));
            for (uint32_t i = 0; i < 128; ++i) write16(data + 1024 + i * 2, g2_read_32(0xa0703000u + i * 4));
            for (uint32_t i = 0; i < 64; ++i) write16(data + 1280 + i * 2, g2_read_32(0xa0703200u + i * 4));
            write16(data + 1408, g2_read_32(0xa0702000u)); write16(data + 1410, g2_read_32(0xa0702004u));
            return reply(fd, opcode, sequence, AFX_OK, data, sizeof(data));
        }
    } else if (opcode == TUNER_DSP_PROGRAM_RING) {
        result = bytes == DSP_PROGRAM_BYTES + 1u ?
            afx_dsp_scene_program_ring(payload + 1, bytes - 1u, payload[0]) : -AFX_BAD_COMMAND;
        if (!result) {
            uint8_t data[DSP_PROGRAM_BYTES];
            for (uint32_t i = 0; i < 512; ++i) write16(data + i * 2, g2_read_32(0xa0703400u + i * 4));
            for (uint32_t i = 0; i < 128; ++i) write16(data + 1024 + i * 2, g2_read_32(0xa0703000u + i * 4));
            for (uint32_t i = 0; i < 64; ++i) write16(data + 1280 + i * 2, g2_read_32(0xa0703200u + i * 4));
            write16(data + 1408, g2_read_32(0xa0702000u)); write16(data + 1410, g2_read_32(0xa0702004u));
            return reply(fd, opcode, sequence, AFX_OK, data, sizeof(data));
        }
    } else if (opcode == TUNER_DSP_DISABLE) {
        result = !bytes ? afx_dsp_scene_disable() : -AFX_BAD_COMMAND;
        trace("TUNER dsp-disable result=%d", result);
    } else if (opcode == TUNER_PATCH) {
        if (!state->instance || bytes < 8) result = -AFX_BAD_COMMAND;
        else {
            uint32_t mask = read32(payload + 4), words = 0;
            for (uint32_t bit = mask; bit; bit >>= 1) words += bit & 1u;
            if (mask & ~AFX_FIELD_MASK || bytes != 8u + words * 2u) result = -AFX_BAD_COMMAND;
            else {
                uint16_t values[AFX_FIELD_COUNT];
                for (uint32_t index = 0; index < words; ++index) values[index] = read16(payload + 8 + index * 2u);
                result = afx_instance_patch(state->instance, payload[0], mask, values);
            }
        }
    } else if (opcode == TUNER_STATUS) {
        afx_instance_status_t current = {0};
        if (bytes) result = -AFX_BAD_COMMAND;
        else if (state->instance) result = afx_instance_status(state->instance, &current);
        write32(status, current.state); write32(status + 4, current.result);
        write32(status + 8, current.position); write32(status + 12, current.detail);
        write32(status + 16, afx_status_timer_ticks()); write32(status + 20, afx_status_heartbeat());
        write32(status + 24, state->asset);
        trace("TUNER status result=%d", result);
        return reply(fd, opcode, sequence, result, status, sizeof(status));
    } else if (opcode == TUNER_REGISTER_READ) {
        uint32_t offset = bytes == 8 ? read32(payload) : UINT32_MAX;
        uint32_t count = bytes == 8 ? read32(payload + 4) : 0;
        if ((offset & 3u) || !count || count > 256u || offset > 0x4680u || count * 4u > 0x4680u - offset)
            result = -AFX_BAD_BOUNDS;
        else {
            uint8_t data[1024];
            for (uint32_t i = 0; i < count; ++i) write32(data + i * 4, g2_read_32(0xa0700000u + offset + i * 4));
            return reply(fd, opcode, sequence, AFX_OK, data, count * 4);
        }
    } else if (opcode == TUNER_ASSET_READ) {
        uint32_t offset = bytes == 8 ? read32(payload) : UINT32_MAX;
        uint32_t count = bytes == 8 ? read32(payload + 4) : 0;
        uint32_t asset_bytes = state->asset ? afx_asset_size(state->asset) : 0;
        if (!state->asset || !count || count > TUNER_MAX_ASSET_READ || offset > asset_bytes ||
            count > asset_bytes - offset) result = -AFX_BAD_BOUNDS;
        else {
            uint8_t data[TUNER_MAX_ASSET_READ];
            spu_memread(data, afx_asset_addr(state->asset) + offset, count);
            trace("TUNER asset-read offset=%lu bytes=%lu", (unsigned long)offset, (unsigned long)count);
            return reply(fd, opcode, sequence, AFX_OK, data, count);
        }
    } else if (opcode == TUNER_EXIT) {
        result = bytes ? -AFX_BAD_COMMAND : stop_instance(state);
        if (!result && state->asset) result = afx_asset_free(state->asset);
        if (!result && state->bank.asset) result = afx_bank_release(&state->bank);
        if (!result) { discard_upload(state); state->asset = 0; exiting = 1; }
        trace("TUNER exit result=%d", result);
    } else result = -AFX_BAD_COMMAND;
    return reply(fd, opcode, sequence, result, NULL, 0);
}
static void serve_client(int fd, tuner_state_t *state) {
    /* KOS does not reliably surface an idle peer close, so one accepted TCP
     * connection deliberately carries exactly one acknowledged command. */
    uint8_t header[TUNER_HEADER_BYTES];
    /* A host-side port probe may connect and immediately disconnect without
     * sending a tuner header. KOS can miss that peer-close notification, so
     * a raw blocking recv here would wedge the whole single-client server. */
    if (io_all(fd, header, sizeof(header), 1)) return;
    uint32_t bytes = read32(header + 12);
    if (read32(header) != TUNER_MAGIC || read16(header + 4) != TUNER_VERSION ||
        (read16(header + 6) & 0x8000u) || bytes > TUNER_MAX_UPLOAD) return;
    uint16_t opcode = read16(header + 6);
    if (opcode != TUNER_UPLOAD_CHUNK && opcode != TUNER_PING && opcode != TUNER_STATUS)
        trace("TUNER recv op=%u bytes=%lu", opcode, (unsigned long)bytes);
    uint8_t *payload = bytes ? malloc(bytes) : NULL;
    if (bytes && !payload) { (void)reply(fd, read16(header + 6), read32(header + 8), -AFX_NO_HOST_RAM, NULL, 0); return; }
    int error = bytes && recv_payload(fd, payload, bytes);
    if (!error) (void)handle(fd, state, header, payload);
    free(payload);
}
int main(void) {
    if (console_init()) return 1;
    if (afx_init(firmware, sizeof(firmware))) { trace("TUNER AFX init failed"); return 1; }
    int server = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(TUNER_PORT), .sin_addr = { .s_addr = htonl(INADDR_ANY) } };
    if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof(address)) || listen(server, 1)) {
        trace("TUNER listen failed (%d)", server); return 1;
    }
    tuner_state_t state = { .music_gain = 255 };
    trace("TUNER READY tcp=%u", TUNER_PORT);
    for (;;) {
        struct pollfd pollfd = { .fd = server, .events = POLLIN };
        if (poll(&pollfd, 1, 20) == 1 && (pollfd.revents & POLLIN)) {
            int client = accept(server, NULL, NULL);
            if (client >= 0) { serve_client(client, &state); close(client); }
        }
        (void)afx_update();
        playback_update(&state);
        present();
        if (exiting) break;
    }
    close(server);
    afx_shutdown();
    trace("TUNER EXITED");
    return 0;
}
