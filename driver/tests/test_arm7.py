"""Run the ARM7 voice/restore logic with host-backed registers and staging RAM."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "driver/arm7/driver.c").read_text()


def function(name):
    start = source.index("static ", source.rfind("\n", 0, source.index(name + "(")) + 1)
    end = source.index("\n}", start) + 2
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <string.h>
#include <aicaflow/codec.h>
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
/* The executor reads its stream from AICA RAM by address; here from a buffer. */
#define STREAM_BASE 0x4000u
static uint8_t stream_bytes[64];
static uint8_t setup_bytes[AFX_SETUP_BYTES];
static afx_observed_t observed_records[AFX_MAX_FLOW_SLOTS];
#define OBSERVED observed_records
static uint32_t last_state, last_sequence, last_position, last_deadline;
static uint32_t loop_position, loop_deadline, loop_publishes;
static int physical_channel(const volatile afx_runtime_slot_t *context,
                            uint32_t local, uint32_t *out) {
    if (local >= context->channels) return 0;
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
names = ("range", "asset_range", "clear_words", "dsp_returns", "dsp_disable", "dsp_nop", "scene_result", "dsp_control", "reference_index",
         "multiply_gain", "apply_gain", "projected_mix", "projected_direct",
         "projected_dsp_send", "write_projection", "release_voice", "keyoff",
         "stream_position", "state_word", "install_voice", "write_restore", "set_instance_gain", "set_lanes", "rebuild",
         "lifecycle", "tick_due", "schedule_wait", "record_lateness", "complete", "write_note",
         "write_patch", "service")
body = "\n".join(function(name) for name in names)
# Keep production validation/installation intact; only translate staging pointers.
assert body.count("(request.states_address +") == 2
body = body.replace("(request.states_address +", "((uintptr_t)restore_states +")
assert body.count("(const uint8_t *)context->pc,") == 1
body = body.replace("(const uint8_t *)context->pc,", "stream_bytes + (context->pc - STREAM_BASE),")
assert body.count("(const uint8_t *)(context->setups + event->setup * AFX_SETUP_BYTES)") == 1
body = body.replace("(const uint8_t *)(context->setups + event->setup * AFX_SETUP_BYTES)", "setup_bytes")
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
    /* A looping stream: WAIT8 2, KEYOFF 0, WAIT8 3, LOOP back to the KEYOFF.
     * The LOOP moves the cursor in the tick it is due; the deadline carries on. */
    (void)setup_bytes;
    memset(stream_bytes, 0, sizeof(stream_bytes));
    stream_bytes[0] = AFX_OP_WAIT8; stream_bytes[1] = 2;
    stream_bytes[2] = AFX_OP_KEYOFF; stream_bytes[3] = 0;
    stream_bytes[4] = AFX_OP_WAIT8; stream_bytes[5] = 3;
    stream_bytes[6] = AFX_OP_LOOP; afx_write32(stream_bytes + 7, 2);
    volatile afx_runtime_slot_t *loop = &contexts[2];
    *loop = (afx_runtime_slot_t){.reference = 3, .state = AFX_RUNNING, .channels = 1,
        .image_base = STREAM_BASE, .stream_start = 0, .pc = STREAM_BASE, .end = STREAM_BASE + 11,
        .flags = AFX_FLAG_LOOP, .tempo_period_q8_8 = 256, .gain = 255};
    voice_flags[0] = VOICE_LATCHED;
    clock_value = 0;
    service(2);
    assert(loop->deadline == 2 && loop->pc == STREAM_BASE + 2 && loop->loops == 0);
    clock_value = 2;
    service(2); /* KEYOFF, WAIT 3 */
    assert(loop->deadline == 5 && loop->pc == STREAM_BASE + 6 && !loop_publishes);
    clock_value = 5;
    service(2); /* LOOP, KEYOFF, WAIT 3: one tick, no gap */
    assert(loop->loops == 1 && loop->deadline == 8 && loop->pc == STREAM_BASE + 6);
    assert(loop_publishes == 1 && loop_position == 2 && loop_deadline == 5);
    assert(last_state == (AFX_RUNNING | (1u << AFX_STATE_LOOPS_SHIFT)) && last_result == AFX_OK);
    clock_value = 8;
    service(2); /* The second loop: the body is WAIT 3 long, so it comes 3 ticks later. */
    assert(loop->loops == 2 && loop->deadline == 11 && loop->pc == STREAM_BASE + 6);
    assert(last_state == (AFX_RUNNING | (2u << AFX_STATE_LOOPS_SHIFT)));
    /* Without the flag, LOOP ends the flow in error. */
    loop->flags = 0;
    clock_value = 11;
    service(2);
    assert(last_state == AFX_ERROR && last_result == AFX_BAD_COMMAND && !loop->reference);
    /* A loop body that never waits (which validation refuses) ends it too. */
    stream_bytes[0] = AFX_OP_KEYOFF; stream_bytes[1] = 0;
    stream_bytes[2] = AFX_OP_LOOP; afx_write32(stream_bytes + 3, 0);
    *loop = (afx_runtime_slot_t){.reference = 3, .state = AFX_RUNNING, .channels = 1,
        .image_base = STREAM_BASE, .stream_start = 0, .pc = STREAM_BASE, .end = STREAM_BASE + 7,
        .flags = AFX_FLAG_LOOP, .tempo_period_q8_8 = 256, .gain = 255};
    clock_value = 0;
    service(2);
    assert(last_state == AFX_ERROR && last_result == AFX_BAD_COMMAND && !loop->reference);
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
print("ARM7 gain, pan, lifecycle races, voice flags, checkpoint restore and LOOP: PASS")
