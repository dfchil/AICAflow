"""Run the ARM7 voice/restore logic with host-backed registers and staging RAM."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "driver/arm7/driver.c").read_text()


def function(name):
    # The definition, not the first use: patch( also ends write_patch(.
    start = re.search(r"^static [^\n(]*\b" + name + r"\(", source, re.M).start()
    end = source.index("\n}", start) + 2
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <aicaflow/codec.h>
#include <stdint.h>
#include <aicaflow/protocol.h>
/* AICA RAM by address: a flow's stream at STREAM_BASE, a song from SONG_BASE. */
#define STREAM_BASE 0x4000u
#define SONG_BASE 0x10000u
static uint8_t stream_bytes[64];
static _Alignas(4) uint8_t song_ram[0x800];
static uintptr_t aica_ptr(uint32_t address) {
    if (address >= SONG_BASE) {
        assert(address - SONG_BASE < sizeof(song_ram));
        return (uintptr_t)(song_ram + (address - SONG_BASE));
    }
    assert(address >= STREAM_BASE && address - STREAM_BASE < sizeof(stream_bytes));
    return (uintptr_t)(stream_bytes + (address - STREAM_BASE));
}
#define AICA_RAM(address) aica_ptr(address)
static uint16_t song_position[AFX_MAX_FLOW_SLOTS];
#include "sample_address.h"
static volatile uint32_t registers[64][32];
#undef AFX_AICA_REG_BASE
#define AFX_AICA_REG_BASE ((uintptr_t)registers)
static volatile afx_runtime_slot_t contexts[AFX_MAX_FLOW_SLOTS];
static afx_restore_channel_t restore_states[2];
static uint32_t dsp_delay_base, dsp_delay_bytes, dsp_owner, last_result;
static uint32_t ring_register, dsp_writes, ring_clears;
#define __asset_base ((uint8_t *)(uintptr_t)0x800)
static void dsp_write(uint32_t offset, uint16_t value) {
    ++dsp_writes;
    if (offset == 0x2804) ring_register = value;
}
static void dsp_clear_delay(void) {
    if (dsp_delay_bytes) ++ring_clears;
    assert(!dsp_delay_bytes || (dsp_delay_base >= 0x800 &&
           dsp_delay_bytes <= AFX_ASSET_MAX - dsp_delay_base));
}
static afx_status_t status;
#define STATUS (&status)
static uint32_t clock_value;
#define CLOCK (&clock_value)
#define AFX_TEMPO_NORMAL 256u
#define AFX_TEMPO_MIN 16u
#define AFX_TEMPO_MAX 4096u
#define AFX_TEMPO_WAIT_CHUNK (UINT32_MAX / AFX_TEMPO_MAX)
static _Alignas(4) uint8_t image_bytes[64]; /* A flow image at STREAM_BASE, for its bank table. */
static uint8_t setup_bytes[AFX_SETUP_BYTES];
static afx_observed_t observed_records[AFX_MAX_FLOW_SLOTS];
#define OBSERVED observed_records
static uint32_t last_state, last_sequence, last_position, last_deadline;
static uint32_t loop_position, loop_deadline, loop_publishes;
static int physical_channel(const volatile afx_runtime_slot_t *context,
                            uint32_t local, uint32_t *out) {
    if (local >= context->channels) return 0;
    if (context->channel_map >= SONG_BASE) { /* A song's map; flows map local i to voice i. */
        uint32_t physical = *(volatile uint8_t *)aica_ptr(context->channel_map + local);
        if (physical >= 64) return 0;
        *out = physical;
        return 1;
    }
    *out = local;
    return 1;
}
static void publish(uint32_t index, uint32_t reference, uint32_t state, uint32_t sequence,
                    uint32_t result, uint32_t position, uint32_t deadline, uint32_t detail) {
    (void)index; (void)reference; (void)detail;
    last_result = result; last_state = state; last_sequence = sequence;
    last_position = position; last_deadline = deadline;
    if ((state >> AFX_STATE_LOOPS_SHIFT) && !loop_publishes++) {
        loop_position = position; loop_deadline = deadline;
    }
}
'''
globals_ = source[source.index("static const uint8_t gain_attenuation"):
                  source.index("static uint32_t dsp_owner")]
globals_ += "\n#define SONG_FIELD(image, field) image_word(image, offsetof(afx_song_image_header_t, field))\n"
names = ("range", "asset_range", "clear_words", "dsp_returns", "dsp_disable", "dsp_nop", "scene_result", "dsp_control", "reference_index",
         "multiply_gain", "apply_gain", "projected_mix", "projected_direct",
         "projected_pitch", "projected_dsp_send", "write_projection", "release_voice", "keyoff",
         "image_word", "song_control", "song_executor", "song_enter",
         "stream_position", "state_word", "install_voice", "write_restore", "pool_take", "pool_give",
         "song_map", "song_unmap", "song_start", "tick_due", "schedule_wait", "record_lateness",
         "complete", "song_next", "set_instance_gain", "set_lanes", "rebuild",
         "lifecycle", "write_note", "write_patch", "patch", "execute", "service", "bank_table_valid")
body = "\n".join(function(name) for name in names)
# Keep production validation/installation intact; only translate staging pointers.
assert body.count("AICA_RAM(request.states_address +") == 2
body = body.replace("AICA_RAM(request.states_address +", "((uintptr_t)restore_states +")
assert body.count("(const uint8_t *)AICA_RAM(context->setups + event->setup * AFX_SETUP_BYTES)") == 1
body = body.replace("(const uint8_t *)AICA_RAM(context->setups + event->setup * AFX_SETUP_BYTES)", "setup_bytes")
assert body.count("AICA_RAM(activate->image_base + table + slot * 4u)") == 1
body = body.replace("AICA_RAM(activate->image_base + table + slot * 4u)",
                    "((uintptr_t)image_bytes + activate->image_base - STREAM_BASE + table + slot * 4u)")
checks = r'''
int main(void) {
    /* All supported sizes, at a non-top address; disable resets ownership. */
    status.asset_limit = AFX_ASSET_MAX;
    for (unsigned code = 0; code < 4; ++code) {
        uint32_t bytes = AFX_DSP_MIN_BYTES << code;
        afx_dsp_payload_t ring = {.ring_address = 0x1800, .ring_bytes = bytes};
        dsp_control(AFX_CMD_DSP_ENABLE, AFX_DSP_SCENE_REFERENCE, 1, 1, (const uint8_t *)&ring);
        assert(status.dsp_result == AFX_OK && dsp_owner == AFX_DSP_SCENE_REFERENCE);
        assert(dsp_delay_bytes == bytes && dsp_delay_base == 0x1800);
        assert(ring_register == ((code << 13) | 3));
        assert(asset_range(0x1000, 0x800) && !asset_range(0x1000, 0x801));
        assert(!asset_range(0x1800, 4) && asset_range(0x1800 + bytes, 4));
        uint32_t writes = dsp_writes;
        ring.ring_address++;
        dsp_control(AFX_CMD_DSP_ENABLE, AFX_DSP_SCENE_REFERENCE, 2, 1, (const uint8_t *)&ring);
        assert(status.dsp_result == AFX_BAD_COMMAND && dsp_writes == writes);
        ring = (afx_dsp_payload_t){0};
        dsp_control(AFX_CMD_DSP_DISABLE, AFX_DSP_SCENE_REFERENCE, 3, 1, (const uint8_t *)&ring);
        assert(status.dsp_result == AFX_OK && !dsp_delay_bytes && !dsp_delay_base && !dsp_owner);
        dsp_control(AFX_CMD_DSP_DISABLE, AFX_DSP_SCENE_REFERENCE, 4, 1, (const uint8_t *)&ring);
        assert(status.dsp_result == AFX_OK); /* Retry after a lost acknowledgement. */
    }
    afx_dsp_payload_t host_ring = {.ring_address = 0x1800, .ring_bytes = AFX_DSP_MIN_BYTES};
    uint32_t clears = ring_clears;
    dsp_control(AFX_CMD_DSP_ENABLE, AFX_DSP_SCENE_REFERENCE, 5,
                1u | AFX_DSP_FLAG_HOST_INIT, (const uint8_t *)&host_ring);
    assert(status.dsp_result == AFX_OK && ring_clears == clears);
    assert(dsp_delay_base == 0x1800 && dsp_delay_bytes == AFX_DSP_MIN_BYTES);
    afx_dsp_payload_t stop_ring = {0};
    dsp_control(AFX_CMD_DSP_DISABLE, AFX_DSP_SCENE_REFERENCE, 6, 1, (const uint8_t *)&stop_ring);
    assert(status.dsp_result == AFX_OK && ring_clears == clears + 1);
    clears = dsp_writes;
    dsp_control(AFX_CMD_DSP_DISABLE, AFX_DSP_SCENE_REFERENCE, 7,
                1u | AFX_DSP_FLAG_HOST_INIT, (const uint8_t *)&stop_ring);
    assert(status.dsp_result == AFX_BAD_COMMAND && dsp_writes == clears);
    const afx_dsp_payload_t invalid[] = {
        {.ring_address = 0x800}, {.ring_bytes = AFX_DSP_MIN_BYTES},
        {.ring_address = 0x800, .ring_bytes = 12345},
        {.ring_address = 0x1fc800, .ring_bytes = AFX_DSP_MIN_BYTES},
        {.reserved = {1}}, {.ring_address = 0x800, .ring_bytes = UINT32_MAX}
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        uint32_t writes = dsp_writes;
        dsp_control(AFX_CMD_DSP_ENABLE, AFX_DSP_SCENE_REFERENCE, 4, 1, (const uint8_t *)&invalid[i]);
        assert(status.dsp_result == AFX_BAD_COMMAND && dsp_writes == writes);
    }
    afx_dsp_payload_t no_ring = {0};
    dsp_control(AFX_CMD_DSP_ENABLE, AFX_DSP_SCENE_REFERENCE, 5, 1, (const uint8_t *)&no_ring);
    assert(status.dsp_result == AFX_OK && !dsp_delay_bytes && !ring_register);
    assert(status.asset_limit == AFX_ASSET_MAX);
    /* Every pan code and signed lane offset, including both centre encodings. */
    for (unsigned code = 0; code < 32; ++code) {
        for (int offset = -128; offset <= 127; ++offset) {
            voice_base_direct[0] = 0x0f00 | code;
            voice_lane_pan[0] = offset;
            int expected = code & 16 ? -(int)(code & 15) : (int)(code & 15);
            expected += offset;
            if (expected < -15) expected = -15;
            if (expected > 15) expected = 15;
            uint16_t actual = projected_direct(0);
            int signed_pan = actual & 16 ? -(int)(actual & 15) : (int)(actual & 15);
            assert(signed_pan == expected && (actual & ~31u) == 0x0f00);
            if (!offset) assert(actual == voice_base_direct[0]);
            voice_flags[0] = VOICE_RELEASED;
            assert(projected_direct(0) == voice_base_direct[0]);
            voice_flags[0] = 0;
        }
    }
    /* Gain scales the attenuation byte only: filter Q and the bypass flags in
     * the low byte reach the register as authored. */
    voice_flags[0] = 0; voice_lane_gain[0] = 255;
    voice_base_mix[0] = 0x2064;
    assert(projected_mix(0, 255) == 0x2064);
    assert((projected_mix(0, 128) & 0xffu) == 0x64 && (projected_mix(0, 128) >> 8) > 0x20);
    assert(projected_mix(0, 0) == 0xff64);
    /* A flow that ended on its own just before a queued STOP or PAUSE arrives
     * reports how it ended, under the new sequence, not a stale handle. */
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t opcode = i ? AFX_CMD_PAUSE : AFX_CMD_STOP;
        contexts[1] = (afx_runtime_slot_t){0};
        observed_records[1] = (afx_observed_t){.reference = 0x10002, .state = AFX_DONE, .result = AFX_OK};
        lifecycle(opcode, 0x10002, 40 + i);
        assert(last_state == AFX_DONE && last_result == AFX_OK && last_sequence == 40 + i);
        lifecycle(opcode, 0x20002, 50 + i); /* a handle that never ran here */
        assert(last_state == AFX_ERROR && last_result == AFX_STALE_GENERATION);
    }
    volatile afx_runtime_slot_t *ctx = &contexts[0];
    *ctx = (afx_runtime_slot_t){.reference = 1, .state = AFX_PAUSED,
        .channels = 2, .image_base = 0x1000, .end = 0x1100, .pc = 0x1020,
        .stream_start = 32, .bank_base_units = 0xffe0u >> 5, .bank_end = 0x10100,
        .gain = 255};
    for (unsigned i = 0; i < 2; ++i) {
        voice_lane_gain[i] = voice_lane_dsp_send[i] = 255;
        voice_lane_pan[i] = 0;
        restore_states[i].local_channel = i;
        restore_states[i].fields[AFX_FIELD_CONTROL] = 0xc200;
        restore_states[i].fields[AFX_FIELD_SAMPLE_LOW] = 32;
        restore_states[i].fields[AFX_FIELD_PITCH] = 0x123;
    }
    afx_rebuild_payload_t request = {.states_address = 0x1000, .state_count = 2,
        .stream_position = 40, .remaining_wait = 17, .next_deadline = 99};
    /* Invalid second address must leave both voices and playback context untouched. */
    restore_states[1].fields[AFX_FIELD_SAMPLE_LOW] = 0x120;
    registers[0][AFX_FIELD_PITCH] = registers[1][AFX_FIELD_PITCH] = 0xabcd;
    rebuild(1, 2, AFX_REBUILD_RUN, (const uint8_t *)&request);
    assert(last_result == AFX_BAD_SAMPLE && ctx->state == AFX_PAUSED && ctx->pc == 0x1020);
    assert(registers[0][AFX_FIELD_PITCH] == 0xabcd && registers[1][AFX_FIELD_PITCH] == 0xabcd);
    assert(!voice_flags[0] && !voice_flags[1]);
    restore_states[1].fields[AFX_FIELD_SAMPLE_LOW] = 32;
    rebuild(1, 3, AFX_REBUILD_RUN, (const uint8_t *)&request);
    assert(last_result == AFX_OK && ctx->state == AFX_RUNNING);
    assert(ctx->pc == 0x1028 && ctx->remaining_wait == 17 && ctx->deadline == 99);
    for (unsigned i = 0; i < 2; ++i) {
        assert(registers[i][AFX_FIELD_CONTROL] == 0xc201);
        assert(registers[i][AFX_FIELD_SAMPLE_LOW] == 0);
        assert(registers[i][AFX_FIELD_PITCH] == 0x123);
        assert(voice_flags[i] == VOICE_LATCHED);
    }
    /* Non-running restore installs registers without key-on. */
    ctx->state = AFX_PAUSED;
    rebuild(1, 4, 0, (const uint8_t *)&request);
    assert(last_result == AFX_OK && ctx->state == AFX_PAUSED);
    assert(registers[0][AFX_FIELD_CONTROL] == 0x201);
    /* Muting/releasing and unmuting preserve independent flags. */
    ctx->flags |= AFX_FLAG_LANES;
    ctx->lane_count = 1;
    /* Gain updates must preserve the adjacent flags and lane count. */
    afx_gain_payload_t gain = {.gain = 128};
    set_instance_gain(1, (const uint8_t *)&gain);
    assert(ctx->gain == 128 && ctx->flags == AFX_FLAG_LANES && ctx->lane_count == 1);
    afx_lane_payload_t lane = {.mask = 1, .values = {1}};
    set_lanes(1, AFX_LANE_MUTE, (const uint8_t *)&lane);
    assert(voice_flags[0] == (VOICE_MUTED | VOICE_LATCHED | VOICE_RELEASED));
    lane.values[0] = 0;
    set_lanes(1, AFX_LANE_MUTE, (const uint8_t *)&lane);
    assert(voice_flags[0] == (VOICE_LATCHED | VOICE_RELEASED));
    assert(write_restore(ctx, &restore_states[0], 1) == AFX_OK);
    assert(voice_flags[0] == VOICE_LATCHED);
    voice_flags[0] |= VOICE_MUTED;
    keyoff(ctx);
    assert(voice_flags[0] == VOICE_MUTED);
    assert(write_restore(ctx, &restore_states[0], 1) == AFX_OK);
    assert(voice_flags[0] == VOICE_MUTED); /* Muted restores do not latch. */
    /* A pitch lane multiplies the word by 1 + value / 256, moving OCT past an octave's end. */
    voice_flags[0] = VOICE_LATCHED;
    voice_base_pitch[0] = 256; /* OCT 0, mantissa 1280 */
    afx_lane_payload_t bend = {.mask = 1, .values = {64}};
    set_lanes(1, AFX_LANE_PITCH, (const uint8_t *)&bend);
    assert(registers[0][AFX_FIELD_PITCH] == 576);
    voice_base_pitch[0] = 512; /* mantissa 1536, times 127 / 256 more: past 2048 */
    bend.values[0] = 127;
    set_lanes(1, AFX_LANE_PITCH, (const uint8_t *)&bend);
    assert(registers[0][AFX_FIELD_PITCH] == ((1u << 11) | 125u));
    voice_base_pitch[0] = 256; /* a half of 1280: below 1024, OCT -1 */
    bend.values[0] = (uint8_t)-128;
    set_lanes(1, AFX_LANE_PITCH, (const uint8_t *)&bend);
    assert(registers[0][AFX_FIELD_PITCH] == ((15u << 11) | 256u));
    bend.values[0] = 0;
    set_lanes(1, AFX_LANE_PITCH, (const uint8_t *)&bend);
    assert(registers[0][AFX_FIELD_PITCH] == 256);
    /* A continue-only song: intro (KEYOFF 0, WAIT 2), then a body (KEYOFF 0,
     * WAIT 3) looping to itself. Each NEXT continue moves the cursor in the
     * tick it is due, on the same executor and voices; the wrap is a loop. */
    (void)setup_bytes;
    memset(song_ram, 0, sizeof(song_ram));
    uint8_t *image = song_ram;
    const uint8_t intro[] = {AFX_OP_KEYOFF, 0, AFX_OP_WAIT8, 2, AFX_OP_NEXT, AFX_JOIN_CONTINUE, AFX_OP_END};
    const uint8_t body[] = {AFX_OP_KEYOFF, 0, AFX_OP_WAIT8, 3, AFX_OP_NEXT, AFX_JOIN_CONTINUE, AFX_OP_END};
    afx_write32(image, 2); afx_write32(image + 4, 2); afx_write32(image + 8, 1);
    afx_write32(image + 12, 0x40); afx_write32(image + 16, 0x58); afx_write32(image + 20, 0x60);
    afx_write32(image + 24, sizeof(intro) + sizeof(body)); afx_write32(image + 28, 1);
    afx_write32(image + 0x40, 0x60); afx_write32(image + 0x44, sizeof(intro)); afx_write32(image + 0x48, 2);
    afx_write32(image + 0x4c, 0x60 + sizeof(intro)); afx_write32(image + 0x50, sizeof(body));
    afx_write32(image + 0x54, 3);
    afx_write16(image + 0x58, 0); afx_write16(image + 0x5a, 1);
    memcpy(image + 0x60, intro, sizeof(intro));
    memcpy(image + 0x60 + sizeof(intro), body, sizeof(body));
    volatile afx_runtime_slot_t *song = &contexts[2];
    *song = (afx_runtime_slot_t){.reference = 3, .state = AFX_RUNNING, .channels = 1,
        .image_base = SONG_BASE, .flags = AFX_FLAG_SONG, .tempo_period_q8_8 = 256, .gain = 255};
    song_enter(song, SONG_BASE, 0);
    song_position[2] = 0;
    loop_publishes = 0;
    voice_flags[0] = VOICE_LATCHED;
    clock_value = 0;
    service(2);
    assert(song->deadline == 2 && song->pc == SONG_BASE + 0x60 + 4);
    clock_value = 2;
    service(2); /* NEXT continue into the body, KEYOFF, WAIT 3: one tick. */
    assert(song_position[2] == 1 && song->deadline == 5 && song->loops == 0 && !loop_publishes);
    clock_value = 5;
    service(2); /* The body's NEXT wraps to the loop index: a loop. */
    assert(song->loops == 1 && song->deadline == 8 && song_position[2] == 1);
    assert(loop_publishes == 1 && loop_position == AFX_SONG_POSITION(1, 0) && loop_deadline == 5);
    assert(last_state == (AFX_RUNNING | (1u << AFX_STATE_LOOPS_SHIFT)));
    clock_value = 8;
    service(2);
    assert(song->loops == 2 && song->deadline == 11);
    /* The position: order position 1, offset 4 in the body's stream. */
    assert(stream_position(2, song) == AFX_SONG_POSITION(1, 4));
    /* NEXT in a flow, or a new-play join in a song without a pool, is refused. */
    song->flags = AFX_FLAG_SONG; song_enter(song, SONG_BASE, 0);
    image[0x60 + 5] = AFX_JOIN_NEW;
    song_position[2] = 0; song->deadline = 11; clock_value = 13;
    service(2);
    assert(last_state == AFX_ERROR && last_result == AFX_BAD_COMMAND && !song->reference);

    /* A song with a pool of two voices (5 and 9): A (KEYOFF 0, WAIT 10, NEXT new,
     * then a tail KEYOFF 0, WAIT 2, END) played A, A, looping to the first. */
    memset(song_ram, 0, sizeof(song_ram));
    const uint8_t a[] = {AFX_OP_KEYOFF, 0, AFX_OP_WAIT8, 10, AFX_OP_NEXT, AFX_JOIN_NEW,
                         AFX_OP_KEYOFF, 0, AFX_OP_WAIT8, 2, AFX_OP_END};
    afx_write32(image, 1); afx_write32(image + 4, 2); afx_write32(image + 8, 0);
    afx_write32(image + 12, 0x40); afx_write32(image + 16, 0x4c); afx_write32(image + 20, 0x50);
    afx_write32(image + 24, sizeof(a)); afx_write32(image + 28, 1);
    afx_write32(image + 0x40, 0x50); afx_write32(image + 0x44, sizeof(a)); afx_write32(image + 0x48, 10);
    afx_write16(image + 0x4c, 0); afx_write16(image + 0x4e, 0);
    memcpy(image + 0x50, a, sizeof(a));
    song_ram[0x100] = 5; song_ram[0x101] = 9; /* The pool. */
    const uint32_t block = SONG_BASE + 0x200;
    *song = (afx_runtime_slot_t){.reference = 3, .state = AFX_RUNNING, .channels = 2,
        .channel_map = SONG_BASE + 0x100, .image_base = SONG_BASE, .pc = block,
        .flags = AFX_FLAG_SONG | AFX_FLAG_POOL, .tempo_period_q8_8 = 256, .gain = 255};
    volatile afx_song_control_t *control = song_control(song);
    control->free_low = 3;
    /* Pause before the first new-play join: executor 1 has never run. Its
     * empty map must not be followed to address 0. */
    song_start(song, 0, 0, 0, 0);
    lifecycle(AFX_CMD_PAUSE, 3, 30);
    assert(song->state == AFX_PAUSED && control->free_low == 3);
    song->state = AFX_RUNNING;
    song_start(song, 0, 0, 0, 0);
    song_position[2] = 0;
    loop_publishes = 0;
    volatile afx_runtime_slot_t *x0 = song_executor(song, 0), *x1 = song_executor(song, 1);
    volatile uint8_t *map0 = (volatile uint8_t *)aica_ptr(block + AFX_SONG_MAP_OFFSET(0));
    volatile uint8_t *map1 = (volatile uint8_t *)aica_ptr(block + AFX_SONG_MAP_OFFSET(1));
    clock_value = 0;
    service(2); /* The first play takes voice 5 at its first event. */
    assert(map0[0] == 5 && map0[1] == AFX_SONG_UNMAPPED && control->free_low == 2 && x0->deadline == 10);
    clock_value = 10;
    service(2); /* NEXT new: the second play starts with voice 9; the first plays its tail. */
    assert(x1->state == AFX_RUNNING && control->newest == 1 && map1[0] == 9 && !control->free_low);
    assert(x0->deadline == 12 && x1->deadline == 20 && song_position[2] == 1);
    assert(stream_position(2, song) == AFX_SONG_POSITION(1, 4));
    clock_value = 12;
    service(2); /* The first play ends: its voice goes back to the pool. */
    assert(x0->state == 0 && map0[0] == AFX_SONG_UNMAPPED && control->free_low == 1);
    assert(song->state == AFX_RUNNING && !loop_publishes);
    clock_value = 20;
    service(2); /* The loop: the first executor plays again, newest. */
    assert(song->loops == 1 && loop_publishes == 1 && control->newest == 0 && x1->deadline == 22);
    service(2); /* It was started by executor 1, so it runs in the next pass. */
    assert(map0[0] == 5 && x0->deadline == 30);
    /* Pause keeps the newest play and frees every voice; rebuild resumes it. */
    lifecycle(AFX_CMD_PAUSE, 3, 40);
    assert(song->state == AFX_PAUSED && control->free_low == 3 && x1->state == 0);
    afx_rebuild_payload_t resume = {.stream_position = AFX_SONG_POSITION(1, 4), .remaining_wait = 0,
                                    .next_deadline = 50};
    rebuild(3, 41, AFX_REBUILD_RUN, (const uint8_t *)&resume);
    assert(last_result == AFX_OK && song->state == AFX_RUNNING && song_position[2] == 1);
    assert(x0->pc == SONG_BASE + 0x50 + 4 && x0->deadline == 50 && control->newest == 0);
    resume.stream_position = AFX_SONG_POSITION(2, 0); /* Past the order. */
    lifecycle(AFX_CMD_PAUSE, 3, 42);
    rebuild(3, 43, AFX_REBUILD_RUN, (const uint8_t *)&resume);
    assert(last_result == AFX_BAD_COMMAND);
    resume.stream_position = AFX_SONG_POSITION(1, sizeof(a)); /* Past the section. */
    rebuild(3, 44, AFX_REBUILD_RUN, (const uint8_t *)&resume);
    assert(last_result == AFX_BAD_COMMAND);
    /* A tail still playing at the next new-play join ends the song in error. */
    resume.stream_position = AFX_SONG_POSITION(0, 0);
    rebuild(3, 45, AFX_REBUILD_RUN, (const uint8_t *)&resume);
    image[0x50 + 9] = 30; /* A tail of 30 ticks. */
    for (clock_value = 50; song->reference && clock_value < 120; ++clock_value) service(2);
    assert(last_state == AFX_ERROR && last_result == AFX_BAD_COMMAND && clock_value - 1 == 70);
    /* An empty pool ends it too. */
    memset(song_ram + 0x200, 0, 0x200);
    image[0x50 + 9] = 2;
    *song = (afx_runtime_slot_t){.reference = 3, .state = AFX_RUNNING, .channels = 2,
        .channel_map = SONG_BASE + 0x100, .image_base = SONG_BASE, .pc = block,
        .flags = AFX_FLAG_SONG | AFX_FLAG_POOL, .tempo_period_q8_8 = 256, .gain = 255};
    song_start(song, 0, 0, 0, 0);
    clock_value = 0;
    service(2);
    assert(last_state == AFX_ERROR && last_result == AFX_NO_CHANNELS);
    /* A PATCH command on a channel that has not sounded holds for its first
     * NOTE, which keeps the patched words instead of its template's, plain and
     * projected alike; the NOTE after it takes the template's again. */
    volatile afx_runtime_slot_t *fresh = &contexts[3];
    *fresh = (afx_runtime_slot_t){.reference = 4, .state = AFX_RUNNING, .channels = 1,
        .setup_count = 1, .bank_base_units = 0xffe0u >> 5, .bank_end = 0x10100, .gain = 255};
    memset(setup_bytes, 0, sizeof(setup_bytes));
    afx_write16(setup_bytes + 2 * AFX_FIELD_CONTROL, 0xc200);
    afx_write16(setup_bytes + 2 * AFX_FIELD_SAMPLE_LOW, 32);
    afx_write16(setup_bytes + 2 * AFX_FIELD_ENV_AD, 0x001f);
    afx_write16(setup_bytes + 2 * AFX_FIELD_DIRECT, 0);
    afx_write16(setup_bytes + 2 * AFX_FIELD_PITCH, 0x100);
    voice_flags[0] = 0; voice_held[0] = 0; voice_lane_pan[0] = 0; voice_lane_pitch[0] = 0;
    voice_lane_gain[0] = voice_lane_dsp_send[0] = 255;
    afx_patch_payload_t set = {.local_channel = 0,
        .mask = (1u << AFX_FIELD_ENV_AD) | (1u << AFX_FIELD_DIRECT)};
    set.values[AFX_FIELD_ENV_AD < AFX_FIELD_DIRECT ? 0 : 1] = 0x0005;
    set.values[AFX_FIELD_ENV_AD < AFX_FIELD_DIRECT ? 1 : 0] = 0x0f00;
    patch(4, 7, (const uint8_t *)&set);
    assert(voice_held[0] == set.mask && registers[0][AFX_FIELD_ENV_AD] == 0x0005);
    afx_event_t note = {.opcode = AFX_OP_NOTE, .channel = 0, .setup = 0};
    assert(write_note(fresh, &note) == AFX_OK);
    assert(registers[0][AFX_FIELD_ENV_AD] == 0x0005 && registers[0][AFX_FIELD_DIRECT] == 0x0f00);
    assert(registers[0][AFX_FIELD_PITCH] == 0x100 && voice_base_direct[0] == 0x0f00);
    assert(!voice_held[0] && voice_flags[0] == VOICE_LATCHED);
    assert(write_note(fresh, &note) == AFX_OK);
    assert(registers[0][AFX_FIELD_ENV_AD] == 0x001f && registers[0][AFX_FIELD_DIRECT] == 0);
    /* A sounding channel is patched as before, and nothing is held. */
    patch(4, 8, (const uint8_t *)&set);
    assert(!voice_held[0] && registers[0][AFX_FIELD_DIRECT] == 0x0f00);
    /* Stopping or pausing forgets what was held, and a flow's own PATCH
     * before its NOTE (no program asked) holds nothing. */
    keyoff(fresh);
    patch(4, 9, (const uint8_t *)&set);
    assert(voice_held[0] == set.mask);
    keyoff(fresh);
    assert(!voice_held[0]);
    afx_event_t authored = {.opcode = AFX_OP_PATCH, .channel = 0, .mask = set.mask,
                            .values = (const uint8_t *)set.values};
    assert(write_patch(fresh, &authored) == AFX_OK && !voice_held[0]);
    /* Gain set before the first NOTE was never lost: the NOTE projects it. */
    keyoff(fresh);
    afx_gain_payload_t silent = {.gain = 0};
    set_instance_gain(4, (const uint8_t *)&silent);
    assert(write_note(fresh, &note) == AFX_OK && registers[0][AFX_FIELD_TOTAL_LEVEL] == 0xff00);

    /* A bank table, at the first word after a 37-byte stream: empty, or each
     * slot a bank inside the asset arena, and inside the image. */
    afx_activation_t banked = {.image_base = STREAM_BASE, .image_size = 56,
                               .stream_offset = 4, .stream_size = 33};
    uint32_t *table = (uint32_t *)(image_bytes + 40);
    assert(bank_table_valid(&banked));
    table[1] = AFX_BANK_TABLE_ENTRY(0x8000, 0x1000);
    table[3] = AFX_BANK_TABLE_ENTRY(AFX_ASSET_MAX - 0x40, 0x40);
    assert(bank_table_valid(&banked));
    table[2] = AFX_BANK_TABLE_ENTRY(0x400, 0x20); /* Below the asset base. */
    assert(!bank_table_valid(&banked));
    table[2] = AFX_BANK_TABLE_ENTRY(AFX_ASSET_MAX - 0x40, 0x41); /* Past the arena. */
    assert(!bank_table_valid(&banked));
    table[2] = 0x8000u >> 5; /* An address without a size. */
    assert(!bank_table_valid(&banked));
    table[2] = 0;
    banked.image_size = 55; /* The table must lie inside the image. */
    assert(!bank_table_valid(&banked));
    banked.image_size = 56; banked.image_base = STREAM_BASE + 2;
    assert(!bank_table_valid(&banked));
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "check.c").write_text(prefix + globals_ + body + checks)
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I" + str(root / "driver/format/include"),
                    "-I" + str(root / "driver/include"),
                    "-I" + str(root / "driver/arm7"),
                    str(path / "check.c"), "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True)
print("ARM7 gain, pan, lifecycle races, voice flags, checkpoint restore, songs (continue and new-play joins, pool, pause and resume), held PATCH and bank tables: PASS")
