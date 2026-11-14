#include <aicaflow/protocol.h>
#include <aicaflow/codec.h>
#include "aica_access.h"
#include "sample_address.h"
#include "aica_access.h"

/* The host backend compiles this file natively (driver/host/src/firmware.c):
 * aica_access.h maps AICA RAM and registers, and the host clears the state
 * below at every boot as crt0.s does. A new static needs adding there.
 *
 * The timer FIQ only advances AFX_AICA_TIMER_TICK_ADDR. Stream work stays in
 * this normal ARM context, which keeps the explicitly reserved FIQ stack free
 * of decoder and register-write pressure. */
static volatile afx_runtime_slot_t contexts[AFX_MAX_FLOW_SLOTS];
/* A song's order position, per instance slot. */
static uint16_t song_position[AFX_MAX_FLOW_SLOTS];
static volatile uint32_t initialized_data_probe = 0x41584632;
/* KOS's linear 0..255 gain to AICA logarithmic TL attenuation table.  It is
 * static so NOTE/PATCH avoid floating point or a runtime table rebuild. */
static const uint8_t gain_attenuation[256] = {
    255,127,111,102,95,90,86,82,79,77,74,72,70,68,66,65,
    63,62,61,59,58,57,56,55,54,53,52,51,50,50,49,48,
    47,47,46,45,45,44,43,43,42,42,41,41,40,40,39,39,
    38,38,37,37,36,36,35,35,34,34,34,33,33,33,32,32,
    31,31,31,30,30,30,29,29,29,28,28,28,27,27,27,27,
    26,26,26,25,25,25,25,24,24,24,24,23,23,23,23,22,
    22,22,22,21,21,21,21,20,20,20,20,20,19,19,19,19,
    18,18,18,18,18,17,17,17,17,17,17,16,16,16,16,16,
    15,15,15,15,15,15,14,14,14,14,14,14,13,13,13,13,
    13,13,12,12,12,12,12,12,11,11,11,11,11,11,11,10,
    10,10,10,10,10,10,9,9,9,9,9,9,9,8,8,8,
    8,8,8,8,8,7,7,7,7,7,7,7,7,6,6,6,
    6,6,6,6,6,5,5,5,5,5,5,5,5,5,4,4,
    4,4,4,4,4,4,4,3,3,3,3,3,3,3,3,3,
    2,2,2,2,2,2,2,2,2,2,1,1,1,1,1,1,
    1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,
};
static uint8_t voice_lane_gain[AFX_AICA_CHANNEL_COUNT];
static uint8_t voice_lane_dsp_send[AFX_AICA_CHANNEL_COUNT];
static uint8_t voice_lane_id[AFX_AICA_CHANNEL_COUNT];
static int8_t voice_lane_pan[AFX_AICA_CHANNEL_COUNT];
static int8_t voice_lane_pitch[AFX_AICA_CHANNEL_COUNT];
enum { VOICE_MUTED = 1u, VOICE_LATCHED = 2u, VOICE_RELEASED = 4u };
static uint8_t voice_flags[AFX_AICA_CHANNEL_COUNT];
/* Fields a PATCH command set on a channel that has not sounded since its
 * instance started: the channel's first NOTE keeps them instead of its
 * template's. The registers hold their values, the bases the projected ones. */
static uint32_t voice_held[AFX_AICA_CHANNEL_COUNT];
/* The AICA channel aperture is write-only for our purposes. These retain the
 * authored state before instance/lane projection and the words needed by KEYOFF. */
static uint16_t voice_control[AFX_AICA_CHANNEL_COUNT];
static uint16_t voice_base_mix[AFX_AICA_CHANNEL_COUNT];
static uint16_t voice_base_pitch[AFX_AICA_CHANNEL_COUNT];
static uint16_t voice_base_direct[AFX_AICA_CHANNEL_COUNT];
static uint16_t voice_base_dsp_send[AFX_AICA_CHANNEL_COUNT];
static uint32_t dsp_owner;
static uint32_t dsp_delay_base, dsp_delay_bytes;
extern void arm_fiq_enable(void);

#define STATUS ((volatile afx_status_t *)AICA_RAM(AFX_STATUS_ADDR))
#define QUEUE ((volatile afx_cmd_queue_t *)AICA_RAM(AFX_QUEUE_ADDR))
#define OBSERVED ((volatile afx_observed_t *)AICA_RAM(AFX_OBSERVED_ADDR))
#define CLOCK ((volatile uint32_t *)AICA_RAM(AFX_AICA_TIMER_TICK_ADDR))

/* Original bus-0 DSP programs; no extracted commercial payloads. */
static void dsp_write(uint32_t offset, uint16_t value) {
    /* AFX stores the meaningful low 16 bits; AICA's register aperture is
     * 32-bit word addressed, as are the per-channel registers below. */
    AICA_REG_WRITE(offset, value);
}
static void dsp_returns(int enabled) {
    dsp_write(0x2000u, enabled ? 0x0f1fu : 0);
    dsp_write(0x2004u, enabled ? 0x0f0fu : 0);
}
static void dsp_clear_delay(void) {
    volatile uint32_t *delay = (volatile uint32_t *)AICA_RAM(dsp_delay_base);
    for (uint32_t word = 0; word < dsp_delay_bytes / sizeof(*delay); ++word) delay[word] = 0;
}
static void dsp_disable(void) {
    dsp_returns(0);
    for (uint32_t word = 0; word < 0x300u; ++word) dsp_write(0x3000u + word * 4u, 0);
    for (uint32_t word = 0; word < 256u; ++word) dsp_write(0x4000u + word * 4u, 0);
    for (uint32_t word = 0; word < 64u; ++word) dsp_write(0x4400u + word * 4u, 0);
    for (uint32_t word = 0; word < 16u; ++word) dsp_write(0x4580u + word * 4u, 0);
    dsp_clear_delay();
    dsp_write(0x2804u, 0);
    dsp_delay_base = dsp_delay_bytes = 0;
    dsp_owner = 0;
}

static void dsp_nop(void) {
    for (uint32_t i = 0; i < 128u; ++i) {
        dsp_write(0x3400u + i * 16u, 0);
        dsp_write(0x3404u + i * 16u, 0x2000u);
        dsp_write(0x3408u + i * 16u, 2);
        dsp_write(0x340cu + i * 16u, 0);
    }
}
static uint32_t stack_free(uint32_t low, uint32_t high) {
    uint32_t current = low;
    while (current < high && *(volatile uint32_t *)AICA_RAM(current) == AFX_STACK_PATTERN)
        current += 4;
    return current - low;
}
static int range(uint32_t offset, uint32_t size, uint32_t limit) {
    return offset <= limit && size <= limit - offset;
}
static int asset_range(uint32_t address, uint32_t size) {
    return range(address, size, AFX_ASSET_MAX) &&
        (!dsp_delay_bytes || address >= dsp_delay_base + dsp_delay_bytes ||
         (address <= dsp_delay_base && size <= dsp_delay_base - address));
}
static void clear_words(volatile uint32_t *words, uint32_t count) {
    while (count--) *words++ = 0;
}
static uint32_t reference_index(uint32_t reference) {
    uint32_t raw = reference & 0xffffu;
    return raw ? raw - 1u : AFX_MAX_FLOW_SLOTS;
}
static int tick_due(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}
#define AFX_TEMPO_NORMAL 256u
#define AFX_TEMPO_MIN 16u
#define AFX_TEMPO_MAX 4096u
#define AFX_TEMPO_WAIT_CHUNK (UINT32_MAX / AFX_TEMPO_MAX)
static void schedule_wait(volatile afx_runtime_slot_t *context, uint32_t wait) {
    uint32_t step = wait > AFX_TEMPO_WAIT_CHUNK ? AFX_TEMPO_WAIT_CHUNK : wait;
    uint32_t scaled = step * context->tempo_period_q8_8 + context->tempo_fraction;
    context->remaining_wait = wait - step;
    context->deadline += scaled >> 8;
    context->tempo_fraction = scaled & 255u;
}
static void record_lateness(volatile afx_runtime_slot_t *context, uint32_t now) {
    uint32_t lateness = now - context->deadline;
    if (lateness > STATUS->max_lateness) STATUS->max_lateness = lateness;
    if (lateness > context->max_lateness) context->max_lateness = lateness;
}
static void publish(uint32_t index, uint32_t reference, uint32_t state, uint32_t sequence,
                    uint32_t result, uint32_t position, uint32_t deadline, uint32_t detail) {
    volatile afx_observed_t *record = &OBSERVED[index];
    uint32_t epoch = record->epoch;
    uint32_t news = record->reference != reference || record->state != state ||
                    record->sequence != sequence || record->result != result;
    epoch = (epoch + 1u) | 1u;
    record->epoch = epoch;
    record->reference = reference;
    record->state = state;
    record->sequence = sequence;
    record->result = result;
    record->position = position;
    record->next_deadline = deadline;
    record->detail = detail;
    record->epoch = epoch + 1u;
    /* News goes to the host's event ring, then by interrupt (see
     * AFX_AICA_MCIPD), after the record, which the ARM7 writes in program
     * order. While the host has not yet cleared an earlier interrupt, raising
     * it changes nothing. */
    if (news) {
        uint32_t head = QUEUE->event_head;
        if (head - QUEUE->event_tail < AFX_EVENT_CAPACITY) {
            QUEUE->events[head & (AFX_EVENT_CAPACITY - 1u)] = (uint8_t)index;
            QUEUE->event_head = head + 1u;
        } else {
            QUEUE->event_overflows = QUEUE->event_overflows + 1u;
        }
        AICA_REG_WRITE(AFX_AICA_MCIPD, AFX_AICA_MCI_SCPU);
    }
}
static int physical_channel(const volatile afx_runtime_slot_t *context,
                            uint32_t local, uint32_t *out);
static int map_valid(uint32_t address, uint32_t count) {
    STATUS->reserved = 0xa1040000u;
    if (address < AFX_CHANNEL_MAP_ARENA_ADDR) return 0;
    uint32_t relative = address - AFX_CHANNEL_MAP_ARENA_ADDR;
    uint32_t arena = relative / AFX_CHANNEL_MAP_ARENA_SIZE;
    uint32_t offset_bytes = relative % AFX_CHANNEL_MAP_ARENA_SIZE;
    if (arena >= AFX_CHANNEL_MAP_ARENAS || offset_bytes % AFX_CHANNEL_MAP_ALLOC_ALIGN ||
        !count || count > AFX_CHANNEL_MAP_ENTRIES ||
        offset_bytes / AFX_CHANNEL_MAP_ENTRY_BYTES + count > AFX_CHANNEL_MAP_ENTRIES)
        return 0;
    uint32_t used_low = 0, used_high = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t physical = *(volatile uint8_t *)AICA_RAM(address + i * AFX_CHANNEL_MAP_ENTRY_BYTES);
        uint32_t *used = physical < 32 ? &used_low : &used_high;
        uint32_t bit = 1u << (physical & 31u);
        if (physical >= AFX_AICA_CHANNEL_COUNT || (*used & bit)) return 0;
        *used |= bit;
    }
    STATUS->reserved = 0xa1050000u;
    return 1;
}
static void release_voice(uint32_t physical) {
    voice_control[physical] = (voice_control[physical] & ~AFX_KEYON) | AFX_KEYON_EXECUTE;
    AICA_REG_WRITE(physical * AFX_AICA_CHANNEL_REG_STRIDE, voice_control[physical]);
}
static void keyoff(const volatile afx_runtime_slot_t *context) {
    for (uint32_t local = 0; local < context->channels; ++local) {
        uint32_t physical;
        if (!physical_channel(context, local, &physical)) continue;
        /* Retirement/pause must not leave an authored release tail playing. */
        AICA_CHANNEL(regs, physical);
        AICA_CHANNEL_SET(regs, AFX_FIELD_TOTAL_LEVEL, 0xff00u | (voice_base_mix[physical] & 0xffu));
        AICA_CHANNEL_SET(regs, AFX_FIELD_ENV_DR, 31);
        release_voice(physical);
        voice_flags[physical] &= VOICE_MUTED;
        voice_held[physical] = 0;
    }
}
static uint8_t multiply_gain(uint8_t first, uint8_t second) {
    /* Rounded product / 255 without pulling __aeabi_uidiv into freestanding ARM7. */
    uint32_t rounded = (uint32_t)first * second + 127u;
    return (uint8_t)((rounded + 1u + ((rounded + 1u) >> 8)) >> 8);
}
static uint16_t apply_gain(uint16_t total_level, uint8_t gain) {
    uint32_t attenuation = (total_level >> 8) + gain_attenuation[gain];
    if (attenuation > 255) attenuation = 255;
    /* Only the attenuation byte is scaled: filter Q and the bypass flags in
     * the low byte are the author's. */
    return (uint16_t)((attenuation << 8) | (total_level & 0xffu));
}
static uint16_t projected_mix(uint32_t physical, uint8_t instance_gain) {
    uint8_t lane_gain = (voice_flags[physical] & VOICE_RELEASED) ? 255 : voice_lane_gain[physical];
    return apply_gain(voice_base_mix[physical], multiply_gain(instance_gain, lane_gain));
}
static uint16_t projected_direct(uint32_t physical) {
    if (voice_flags[physical] & VOICE_RELEASED) return voice_base_direct[physical];
    uint16_t direct = voice_base_direct[physical];
    if (!voice_lane_pan[physical]) return direct; /* Preserve either centre encoding. */
    int pan = direct & 15u;
    if (direct & 16u) pan = -pan;
    pan += voice_lane_pan[physical];
    if (pan < -15) pan = -15;
    if (pan > 15) pan = 15;
    uint16_t encoded = pan < 0 ? (uint16_t)(16 - pan) : (uint16_t)pan;
    return (uint16_t)((direct & ~31u) | encoded);
}
/* The pitch word times 1 + offset / 256. FNS, the low 10 bits, is the fraction of a mantissa from
 * 1 to 2, and OCT above it a signed exponent, so a product that leaves the octave moves OCT. */
static uint16_t projected_pitch(uint32_t physical) {
    uint16_t word = voice_base_pitch[physical];
    int offset = voice_lane_pitch[physical];
    if (!offset) return word;
    int mantissa = 1024 + (word & 1023u);
    int octave = (word >> 11) & 15u;
    if (octave > 7) octave -= 16;
    mantissa += (mantissa * offset) >> 8;
    if (mantissa >= 2048) {
        if (octave == 7) return word;
        mantissa >>= 1;
        octave++;
    } else if (mantissa < 1024) {
        if (octave == -8) return word;
        mantissa <<= 1;
        octave--;
    }
    return (uint16_t)((word & ~0x7bffu) | (((uint32_t)octave & 15u) << 11) | ((uint32_t)mantissa - 1024u));
}
static uint16_t projected_dsp_send(uint32_t physical) {
    if (voice_flags[physical] & VOICE_RELEASED) return voice_base_dsp_send[physical];
    uint32_t level = (voice_base_dsp_send[physical] >> 4) & 15u;
    level = multiply_gain((uint8_t)level, voice_lane_dsp_send[physical]);
    return (uint16_t)((voice_base_dsp_send[physical] & ~0xf0u) | (level << 4));
}
static void write_projection(uint32_t physical, uint32_t fields, uint8_t instance_gain) {
    AICA_CHANNEL(regs, physical);
    if (fields & (1u << AFX_FIELD_TOTAL_LEVEL))
        AICA_CHANNEL_SET(regs, AFX_FIELD_TOTAL_LEVEL, projected_mix(physical, instance_gain));
    if (fields & (1u << AFX_FIELD_DIRECT)) AICA_CHANNEL_SET(regs, AFX_FIELD_DIRECT, projected_direct(physical));
    if (fields & (1u << AFX_FIELD_PITCH)) AICA_CHANNEL_SET(regs, AFX_FIELD_PITCH, projected_pitch(physical));
    if (fields & (1u << AFX_FIELD_DSP_SEND))
        AICA_CHANNEL_SET(regs, AFX_FIELD_DSP_SEND, projected_dsp_send(physical));
}
static int physical_channel(const volatile afx_runtime_slot_t *context,
                            uint32_t local, uint32_t *out) {
    if (local >= context->channels) return 0;
    uint32_t physical = *(volatile uint8_t *)AICA_RAM(context->channel_map +
                                                      local * AFX_CHANNEL_MAP_ENTRY_BYTES);
    if (physical >= AFX_AICA_CHANNEL_COUNT) return 0;
    *out = physical;
    return 1;
}
/* Songs. A song's instance slot holds what applies to the whole song. A
 * continue-only song plays in that slot like a flow; a song with a pool keeps
 * its runtime block's address in pc and plays on the block's two executors. */
static uint32_t image_word(uint32_t image, uint32_t field) {
    return *(volatile uint32_t *)AICA_RAM(image + field);
}
#define SONG_FIELD(image, field) image_word(image, offsetof(afx_song_image_header_t, field))
static volatile afx_song_control_t *song_control(const volatile afx_runtime_slot_t *song) {
    return (volatile afx_song_control_t *)AICA_RAM(song->pc);
}
static volatile afx_runtime_slot_t *song_executor(const volatile afx_runtime_slot_t *song, uint32_t e) {
    return (volatile afx_runtime_slot_t *)AICA_RAM(song->pc + AFX_SONG_EXECUTOR_OFFSET(e));
}
/* Points an executor at the section the order plays at position. */
static void song_enter(volatile afx_runtime_slot_t *x, uint32_t image, uint32_t position) {
    uint32_t section = *(volatile uint16_t *)AICA_RAM(image + SONG_FIELD(image, order_offset) + position * 2u);
    uint32_t entry = image + SONG_FIELD(image, sections_offset) + section * AFX_SONG_SECTION_BYTES;
    x->stream_start = *(volatile uint32_t *)AICA_RAM(entry);
    x->pc = image + x->stream_start;
    x->end = x->pc + *(volatile uint32_t *)AICA_RAM(entry + 4u);
}
/* Image-relative for a flow; for a song, its order position and the stream
 * offset of the executor playing the newest section (AFX_SONG_POSITION). */
static uint32_t stream_position(uint32_t index, const volatile afx_runtime_slot_t *context) {
    if (!(context->flags & AFX_FLAG_SONG)) return context->pc - context->image_base;
    const volatile afx_runtime_slot_t *x = context;
    if (context->flags & AFX_FLAG_POOL) x = song_executor(context, song_control(context)->newest);
    return AFX_SONG_POSITION(song_position[index], x->pc - x->image_base - x->stream_start);
}
static void complete(uint32_t index, volatile afx_runtime_slot_t *context,
                     uint32_t state, uint32_t result, uint32_t detail) {
    uint32_t reference = context->reference;
    uint32_t sequence = context->sequence;
    uint32_t position = stream_position(index, context);
    uint32_t deadline = context->deadline;
    uint32_t max_lateness = context->max_lateness;
    keyoff(context);
    clear_words((volatile uint32_t *)context, sizeof(*context) / 4);
    if (result == AFX_OK) detail = max_lateness;
    publish(index, reference, state, sequence, result, position, deadline, detail);
}
/* The state word of a live context: its loop count above its state, so a
 * LOOP is news to the host (AFX_STATE_LOOPS_SHIFT). */
static uint32_t state_word(const volatile afx_runtime_slot_t *context) {
    return (uint32_t)context->state | ((uint32_t)context->loops << AFX_STATE_LOOPS_SHIFT);
}
/* Install a complete NOTE/RESTORE state; key-on is always the final write. */
static afx_result_t install_voice(const volatile afx_runtime_slot_t *context,
                                  uint32_t physical, uint16_t words[AFX_FIELD_COUNT], int run) {
    afx_result_t result = resolve_sample_address(context, &words[AFX_FIELD_CONTROL],
                                                &words[AFX_FIELD_SAMPLE_LOW]);
    if (result) return result;
    uint32_t held = voice_held[physical];
    voice_held[physical] = 0;
    voice_flags[physical] = (voice_flags[physical] & VOICE_MUTED) | VOICE_LATCHED;
    if (!(held & (1u << AFX_FIELD_TOTAL_LEVEL))) voice_base_mix[physical] = words[AFX_FIELD_TOTAL_LEVEL];
    if (!(held & (1u << AFX_FIELD_DIRECT))) voice_base_direct[physical] = words[AFX_FIELD_DIRECT];
    if (!(held & (1u << AFX_FIELD_DSP_SEND))) voice_base_dsp_send[physical] = words[AFX_FIELD_DSP_SEND];
    if (!(held & (1u << AFX_FIELD_PITCH))) voice_base_pitch[physical] = words[AFX_FIELD_PITCH];
    words[AFX_FIELD_TOTAL_LEVEL] = projected_mix(physical, context->gain);
    words[AFX_FIELD_DIRECT] = projected_direct(physical);
    words[AFX_FIELD_PITCH] = projected_pitch(physical);
    words[AFX_FIELD_DSP_SEND] = projected_dsp_send(physical);
    /* The projected words come from the bases; the other held ones are in the registers. */
    held &= ~((1u << AFX_FIELD_TOTAL_LEVEL) | (1u << AFX_FIELD_DIRECT) |
              (1u << AFX_FIELD_DSP_SEND) | (1u << AFX_FIELD_PITCH));
    AICA_CHANNEL(regs, physical);
    uint16_t control = words[AFX_FIELD_CONTROL] & ~(AFX_KEYON | AFX_KEYON_EXECUTE);
    AICA_CHANNEL_SET(regs, AFX_FIELD_CONTROL, control);
    for (uint32_t field = 1; field < AFX_FIELD_COUNT; ++field)
        if (!(held & (1u << field))) AICA_CHANNEL_SET(regs, field, words[field]);
    if (run) control |= AFX_KEYON | AFX_KEYON_EXECUTE;
    voice_control[physical] = control;
    if (run) AICA_CHANNEL_SET(regs, AFX_FIELD_CONTROL, control);
    return AFX_OK;
}
static afx_result_t write_note(const volatile afx_runtime_slot_t *context,
                               const afx_event_t *event) {
    uint32_t physical;
    uint16_t state[AFX_FIELD_COUNT];
    if (event->setup >= context->setup_count ||
        !physical_channel(context, event->channel, &physical)) return AFX_BAD_COMMAND;
    if ((context->flags & AFX_FLAG_LANES) && (voice_flags[physical] & VOICE_MUTED)) return AFX_OK;
    afx_result_t result = afx_apply_setup_fields(state,
        (const uint8_t *)AICA_RAM(context->setups + event->setup * AFX_SETUP_BYTES), event->mask,
        event->values, afx_field_value_bytes(event->mask));
    if (result) return result;
    return install_voice(context, physical, state, 1);
}

static afx_result_t write_patch(const volatile afx_runtime_slot_t *context,
                                const afx_event_t *event) {
    uint32_t physical;
    if (!physical_channel(context, event->channel, &physical)) return AFX_BAD_COMMAND;
    AICA_CHANNEL(regs, physical);
    const uint8_t *value = event->values;
    uint32_t projection = 0;
    for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field) {
        if (!(event->mask & (1u << field))) continue;
        uint16_t field_value = afx_read16(value);
        value += 2;
        if (field == AFX_FIELD_TOTAL_LEVEL) {
            voice_base_mix[physical] = field_value;
            projection |= 1u << field;
        } else if (field == AFX_FIELD_DIRECT) {
            voice_base_direct[physical] = field_value;
            projection |= 1u << field;
        } else if (field == AFX_FIELD_DSP_SEND) {
            voice_base_dsp_send[physical] = field_value;
            projection |= 1u << field;
        } else if (field == AFX_FIELD_PITCH) {
            voice_base_pitch[physical] = field_value;
            projection |= 1u << field;
        } else AICA_CHANNEL_SET(regs, field, field_value);
    }
    write_projection(physical, projection, context->gain);
    return AFX_OK;
}
static afx_result_t write_restore(const volatile afx_runtime_slot_t *context,
                                  const afx_restore_channel_t *state, int run) {
    uint32_t physical;
    if (!physical_channel(context, state->local_channel, &physical)) return AFX_BAD_COMMAND;
    if ((context->flags & AFX_FLAG_LANES) && (voice_flags[physical] & VOICE_MUTED)) return AFX_OK;
    uint16_t words[AFX_FIELD_COUNT];
    for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field) words[field] = state->fields[field];
    return install_voice(context, physical, words, run);
}

/* The pool: entry i of the song's channel map is a voice, free while bit i of
 * the control block's mask is set. */
static uint32_t pool_take(const volatile afx_runtime_slot_t *song) {
    volatile afx_song_control_t *control = song_control(song);
    for (uint32_t i = 0; i < song->channels; ++i) {
        volatile uint32_t *mask = i < 32 ? &control->free_low : &control->free_high;
        uint32_t bit = 1u << (i & 31u);
        if (*mask & bit) {
            *mask &= ~bit;
            return *(volatile uint8_t *)AICA_RAM(song->channel_map + i * AFX_CHANNEL_MAP_ENTRY_BYTES);
        }
    }
    return AFX_SONG_UNMAPPED;
}
static void pool_give(const volatile afx_runtime_slot_t *song, uint32_t physical) {
    volatile afx_song_control_t *control = song_control(song);
    for (uint32_t i = 0; i < song->channels; ++i)
        if (*(volatile uint8_t *)AICA_RAM(song->channel_map + i * AFX_CHANNEL_MAP_ENTRY_BYTES) == physical) {
            if (i < 32) control->free_low |= 1u << i;
            else control->free_high |= 1u << (i - 32u);
            return;
        }
}
/* A local channel's voice, taken from the pool at its first event in a play,
 * with the lane values the song has for its lane. */
static int song_map(const volatile afx_runtime_slot_t *song, volatile afx_runtime_slot_t *x,
                    uint32_t local) {
    if (local >= x->channels) return 1; /* Refused by the instruction itself. */
    volatile uint8_t *map = (volatile uint8_t *)AICA_RAM(x->channel_map);
    if (map[local] != AFX_SONG_UNMAPPED) return 1;
    uint32_t physical = pool_take(song);
    if (physical >= AFX_AICA_CHANNEL_COUNT) return 0;
    map[local] = (uint8_t)physical;
    uint32_t lane = (song->flags & AFX_FLAG_LANES) ?
        *(volatile uint8_t *)AICA_RAM(song->setups + song->setup_count * AFX_SETUP_BYTES + local) : 0;
    voice_lane_id[physical] = (uint8_t)lane;
    voice_control[physical] = 0;
    voice_held[physical] = 0;
    if (song->flags & AFX_FLAG_LANES) {
        volatile uint8_t *values = (volatile uint8_t *)AICA_RAM(song->pc + AFX_SONG_LANES_OFFSET);
        voice_lane_gain[physical] = values[AFX_LANE_GAIN * 64u + lane];
        voice_flags[physical] = values[AFX_LANE_MUTE * 64u + lane] ? VOICE_MUTED : 0;
        voice_lane_pan[physical] = (int8_t)values[AFX_LANE_PAN * 64u + lane];
        voice_lane_dsp_send[physical] = values[AFX_LANE_DSP_SEND * 64u + lane];
        voice_lane_pitch[physical] = (int8_t)values[AFX_LANE_PITCH * 64u + lane];
    } else {
        voice_lane_gain[physical] = voice_lane_dsp_send[physical] = 255;
        voice_lane_pan[physical] = voice_lane_pitch[physical] = 0;
        voice_flags[physical] = 0;
    }
    return 1;
}
/* Gives an executor's voices back to the pool; with silence, keys them off as
 * an ending flow's are. */
static void song_unmap(const volatile afx_runtime_slot_t *song, volatile afx_runtime_slot_t *x,
                       int silence) {
    if (!x->channel_map) return; /* Never started. */
    volatile uint8_t *map = (volatile uint8_t *)AICA_RAM(x->channel_map);
    if (silence) keyoff(x);
    for (uint32_t local = 0; local < AFX_MAX_FLOW_CHANNELS; ++local) {
        if (map[local] != AFX_SONG_UNMAPPED) pool_give(song, map[local]);
        map[local] = AFX_SONG_UNMAPPED;
    }
}
/* Starts executor e on the section at an order position, at deadline. */
static void song_start(const volatile afx_runtime_slot_t *song, uint32_t e, uint32_t position,
                       uint32_t deadline, uint32_t fraction) {
    volatile afx_runtime_slot_t *x = song_executor(song, e);
    volatile uint32_t *map = (volatile uint32_t *)AICA_RAM(song->pc + AFX_SONG_MAP_OFFSET(e));
    for (uint32_t word = 0; word < 16u; ++word) map[word] = 0xffffffffu;
    x->channel_map = song->pc + AFX_SONG_MAP_OFFSET(e);
    x->channels = (uint8_t)SONG_FIELD(song->image_base, channels);
    x->setups = song->setups;
    x->setup_count = song->setup_count;
    x->bank_base_units = song->bank_base_units;
    x->bank_end = song->bank_end;
    x->flags = song->flags;
    x->gain = song->gain;
    x->lane_count = song->lane_count;
    x->reference = song->reference;
    x->sequence = song->sequence;
    x->image_base = song->image_base;
    x->tempo_period_q8_8 = song->tempo_period_q8_8;
    x->tempo_fraction = (uint8_t)fraction;
    x->deadline = deadline;
    x->remaining_wait = 0;
    x->max_lateness = 0;
    x->loops = 0;
    song_enter(x, song->image_base, position);
    x->state = AFX_RUNNING;
}
/* NEXT: the song moves to its next order position, wrapping to its loop
 * index (a loop) or, without one, playing no further section. */
static afx_result_t song_next(uint32_t index, volatile afx_runtime_slot_t *song,
                              volatile afx_runtime_slot_t *x, uint32_t e, uint32_t join) {
    if (!(song->flags & AFX_FLAG_SONG)) return AFX_BAD_COMMAND;
    uint32_t image = song->image_base;
    uint32_t next = song_position[index] + 1u, wrapped = 0;
    if (next >= SONG_FIELD(image, order_count)) {
        next = SONG_FIELD(image, loop_index);
        wrapped = 1;
    }
    if (next == AFX_SONG_NO_LOOP) return AFX_OK; /* The tail plays on to END. */
    if (join == AFX_JOIN_CONTINUE) song_enter(x, image, next);
    else {
        if (!(song->flags & AFX_FLAG_POOL) || song_executor(song, e ^ 1u)->state == AFX_RUNNING)
            return AFX_BAD_COMMAND;
        song_start(song, e ^ 1u, next, x->deadline, x->tempo_fraction);
        song_control(song)->newest = e ^ 1u;
    }
    song_position[index] = (uint16_t)next;
    if (wrapped) {
        song->loops++;
        publish(index, song->reference, state_word(song), song->sequence, AFX_OK,
                stream_position(index, song), x->deadline, song->max_lateness);
    }
    return AFX_OK;
}
/* Runs one executor while it is due: a flow's or a continue-only song's
 * (x == context) or one of a pool song's. */
static void execute(uint32_t index, volatile afx_runtime_slot_t *context,
                    volatile afx_runtime_slot_t *x, uint32_t e) {
    uint32_t events = 0;
    int looped = 0, pool = (context->flags & AFX_FLAG_POOL) != 0;
    while (events < AFX_EXECUTOR_MAX_EVENTS_PER_PASS) {
        uint32_t now = *CLOCK;
        if (!tick_due(now, x->deadline)) return;
        record_lateness(x, now);
        if (x != context && x->max_lateness > context->max_lateness)
            context->max_lateness = x->max_lateness;
        /* Split a legal very long WAIT32 into rollover-safe deadline steps. */
        if (x->remaining_wait) {
            schedule_wait(x, x->remaining_wait);
            continue;
        }
        if (x->pc >= x->end) {
            complete(index, context, AFX_ERROR, AFX_BAD_BOUNDS, now - x->deadline);
            return;
        }
        afx_event_t event;
        afx_result_t result = afx_decode_event((const uint8_t *)AICA_RAM(x->pc), x->end - x->pc, &event);
        if (result) {
            complete(index, context, AFX_ERROR, result, now - x->deadline);
            return;
        }
        x->pc += event.bytes;
        ++events;
        if (pool && event.opcode >= AFX_OP_NOTE && event.opcode <= AFX_OP_KEYOFF &&
            !song_map(context, x, event.channel)) {
            complete(index, context, AFX_ERROR, AFX_NO_CHANNELS, event.channel);
            return;
        }
        switch (event.opcode) {
        case AFX_OP_WAIT8:
        case AFX_OP_WAIT16:
        case AFX_OP_WAIT32: {
            schedule_wait(x, event.wait);
            looped = 0;
            break;
        }
        case AFX_OP_NEXT:
            /* A continue join moves the cursor in this same tick, as LOOP did;
             * a second one before a WAIT (which validation refuses) would spin. */
            result = looped ? AFX_BAD_COMMAND : song_next(index, context, x, e, event.join);
            looped = event.join == AFX_JOIN_CONTINUE;
            break;
        case AFX_OP_FREE: {
            if (!(context->flags & AFX_FLAG_SONG) || event.channel >= x->channels) result = AFX_BAD_COMMAND;
            else if (pool) {
                volatile uint8_t *map = (volatile uint8_t *)AICA_RAM(x->channel_map);
                uint32_t physical = map[event.channel];
                if (physical != AFX_SONG_UNMAPPED) {
                    release_voice(physical);
                    voice_flags[physical] &= VOICE_MUTED;
                    pool_give(context, physical);
                    map[event.channel] = AFX_SONG_UNMAPPED;
                }
            }
            break;
        }
        case AFX_OP_NOTE:
            result = write_note(x, &event);
            break;
        case AFX_OP_PATCH:
            result = write_patch(x, &event);
            break;
        case AFX_OP_KEYOFF: {
            uint32_t physical;
            if (!physical_channel(x, event.channel, &physical)) result = AFX_BAD_COMMAND;
            else {
                release_voice(physical);
                /* KEYOFF may begin an audible release; later authored TOTAL_LEVEL
                 * automation retains the NOTE's instance/lane projection until a
                 * new NOTE replaces it or the instance is explicitly retired. */
            }
            break;
        }
        case AFX_OP_END:
            if (x == context) complete(index, context, AFX_DONE, AFX_OK, now - x->deadline);
            else {
                /* This play is over; the song ends with its last one. */
                song_unmap(context, x, 1);
                x->state = 0;
                if (song_executor(context, e ^ 1u)->state != AFX_RUNNING)
                    complete(index, context, AFX_DONE, AFX_OK, now - x->deadline);
            }
            return;
        case AFX_OP_PARK:
            if (x != context || !(context->flags & AFX_FLAG_CONTROLLED)) {
                complete(index, context, AFX_ERROR, AFX_BAD_COMMAND, now - x->deadline);
                return;
            }
            context->state = AFX_PARKED;
            publish(index, context->reference, state_word(context), context->sequence, AFX_OK,
                    stream_position(index, context), context->deadline, now - context->deadline);
            return;
        default:
            result = AFX_BAD_COMMAND;
            break;
        }
        if (result) {
            complete(index, context, AFX_ERROR, result, now - x->deadline);
            return;
        }
    }
    publish(index, context->reference, state_word(context), context->sequence, AFX_OK,
            stream_position(index, context), x->deadline, context->max_lateness);
}
static void service(uint32_t index) {
    volatile afx_runtime_slot_t *context = &contexts[index];
    if (context->state != AFX_RUNNING) return;
    if (!(context->flags & AFX_FLAG_POOL)) {
        execute(index, context, context, 0);
        return;
    }
    for (uint32_t e = 0; e < 2u && context->state == AFX_RUNNING; ++e) {
        volatile afx_runtime_slot_t *x = song_executor(context, e);
        if (x->state == AFX_RUNNING) execute(index, context, x, e);
    }
}
/* An AFX_FLAG_BANKS flow's bank table: inside the image, and every slot it
 * fills a bank in the asset arena. Slot 0 comes from the activation. */
static int bank_table_valid(const afx_activation_t *activate) {
    uint32_t table = AFX_BANK_TABLE_OFFSET(activate->stream_offset + activate->stream_size);
    if ((activate->image_base & 3u) || !range(table, AFX_BANK_TABLE_BYTES, activate->image_size))
        return 0;
    for (uint32_t slot = 1; slot < AFX_MAX_FLOW_BANKS; ++slot) {
        uint32_t entry = *(volatile uint32_t *)AICA_RAM(activate->image_base + table + slot * 4u);
        uint32_t base = (entry & 0xffffu) << AFX_BANK_ADDRESS_UNIT_SHIFT;
        uint32_t bytes = (entry >> 16) << AFX_BANK_ADDRESS_UNIT_SHIFT;
        if (bytes ? base < AICA_ASSET_BASE || !asset_range(base, bytes) : base)
            return 0;
    }
    return 1;
}
/* A song's image header and tables, read from AICA RAM: every table inside
 * the image, every section inside the streams the activation names, every
 * order entry a section, and the map one entry per local channel (a pool
 * song's lists its pool instead). */
static int song_valid(const afx_activation_t *activate) {
    uint32_t image = activate->image_base, size = activate->image_size;
    if ((image & 3u) || size < AFX_SONG_IMAGE_HEADER_BYTES ||
        activate->setups_offset != AFX_SONG_IMAGE_HEADER_BYTES) return 0;
    uint32_t sections = SONG_FIELD(image, section_count), order = SONG_FIELD(image, order_count);
    uint32_t loop = SONG_FIELD(image, loop_index), channels = SONG_FIELD(image, channels);
    uint32_t table = SONG_FIELD(image, sections_offset), order_at = SONG_FIELD(image, order_offset);
    uint32_t lanes_end = activate->setups_offset + activate->setup_count * AFX_SETUP_BYTES +
                         ((activate->flags & AFX_FLAG_LANES) ? channels : 0);
    if (!sections || !order || order > AFX_SONG_MAX_ORDER || (loop != AFX_SONG_NO_LOOP && loop >= order) ||
        !channels || channels > AFX_MAX_FLOW_CHANNELS || (table & 3u) || (order_at & 1u) ||
        table < lanes_end || !range(table, sections * AFX_SONG_SECTION_BYTES, size) ||
        !range(order_at, order * 2u, size) ||
        SONG_FIELD(image, streams_offset) != activate->stream_offset ||
        SONG_FIELD(image, streams_size) != activate->stream_size ||
        (!(activate->flags & AFX_FLAG_POOL) && activate->required_channels != channels)) return 0;
    if (activate->flags & AFX_FLAG_LANES)
        for (uint32_t local = 0; local < channels; ++local)
            if (*(volatile uint8_t *)AICA_RAM(image + activate->setups_offset +
                                      activate->setup_count * AFX_SETUP_BYTES + local) >= AFX_MAX_FLOW_CHANNELS)
                return 0;
    for (uint32_t i = 0; i < sections; ++i) {
        uint32_t entry = image + table + i * AFX_SONG_SECTION_BYTES;
        uint32_t offset = *(volatile uint32_t *)AICA_RAM(entry), bytes = *(volatile uint32_t *)AICA_RAM(entry + 4u);
        if (!bytes || offset < activate->stream_offset ||
            !range(offset - activate->stream_offset, bytes, activate->stream_size)) return 0;
    }
    for (uint32_t i = 0; i < order; ++i)
        if (*(volatile uint16_t *)AICA_RAM(image + order_at + i * 2u) >= sections) return 0;
    return 1;
}
static void activation(uint32_t reference, uint32_t sequence, const uint8_t payload[48]) {
    STATUS->reserved = 0xa1010000u;
    afx_activation_t activate;
    uint32_t index = reference_index(reference);
    if (index >= AFX_MAX_FLOW_SLOTS) return;
    /* Byte access is alias-safe for the mixed-width activation fields. */
    for (uint32_t i = 0; i < sizeof(activate); ++i)
        ((uint8_t *)&activate)[i] = payload[i];
    STATUS->reserved = 0xa1020000u;

    STATUS->reserved = 0xa1030000u;
    uint32_t bank_base = (uint32_t)activate.bank_base_units << AFX_BANK_ADDRESS_UNIT_SHIFT;
    int song = (activate.flags & AFX_FLAG_SONG) != 0, pool = (activate.flags & AFX_FLAG_POOL) != 0;
    uint32_t runtime = (uint32_t)activate.runtime_units << 5;
    if (contexts[index].reference || (pool && !song) || !pool != !activate.runtime_units ||
        (pool && (runtime < AICA_ASSET_BASE ||
                  !asset_range(runtime, AFX_SONG_RUNTIME_BYTES(activate.flags & AFX_FLAG_LANES)))) ||
        (!activate.bank_bytes && bank_base) ||
        (activate.bank_bytes && (bank_base < AICA_ASSET_BASE ||
         !asset_range(bank_base, activate.bank_bytes))) ||
        !activate.required_channels || activate.required_channels > AFX_MAX_FLOW_CHANNELS ||
        !asset_range(activate.image_base, activate.image_size) ||
        !range(activate.stream_offset, activate.stream_size, activate.image_size) ||
        !activate.stream_size || (activate.setups_offset & 1u) ||
        activate.setup_count > 65536u ||
        !range(activate.setups_offset, activate.setup_count * AFX_SETUP_BYTES, activate.image_size) ||
        (!song && (activate.flags & AFX_FLAG_LANES) &&
         (activate.setups_offset + activate.setup_count * AFX_SETUP_BYTES > activate.stream_offset ||
          activate.required_channels > activate.stream_offset -
              (activate.setups_offset + activate.setup_count * AFX_SETUP_BYTES))) ||
        !map_valid(activate.channel_map, activate.required_channels) ||
        (activate.flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_MUSIC | AFX_FLAG_MUSIC_CHORUS |
                            AFX_FLAG_LANES | AFX_FLAG_BANKS | AFX_FLAG_SONG | AFX_FLAG_POOL)) ||
        ((activate.flags & AFX_FLAG_BANKS) && !bank_table_valid(&activate)) ||
        (song && !song_valid(&activate))) {
        publish(index, reference, AFX_ERROR, sequence, AFX_BAD_COMMAND, 0, 0, 0);
        return;
    }
    STATUS->reserved = 0xa1060000u;
    volatile afx_runtime_slot_t *context = &contexts[index];
    context->pc = activate.image_base + activate.stream_offset;
    context->end = context->pc + activate.stream_size;
    context->setups = activate.image_base + activate.setups_offset;
    context->setup_count = activate.setup_count;
    context->channel_map = activate.channel_map;
    context->channels = activate.required_channels;
    context->bank_base_units = activate.bank_base_units;
    context->bank_end = bank_base + activate.bank_bytes;
    context->deadline = activate.start_tick;
    context->remaining_wait = 0;
    uint32_t lane_count = 0;
    uint32_t lane_map = activate.image_base + activate.setups_offset +
                        activate.setup_count * AFX_SETUP_BYTES;
    /* A pool song's lanes follow its local channels, which take voices later. */
    if (pool && (activate.flags & AFX_FLAG_LANES))
        for (uint32_t local = 0; local < SONG_FIELD(activate.image_base, channels); ++local)
            if (*(volatile uint8_t *)AICA_RAM(lane_map + local) + 1u > lane_count)
                lane_count = *(volatile uint8_t *)AICA_RAM(lane_map + local) + 1u;
    for (uint32_t local = 0; local < activate.required_channels; ++local) {
        uint32_t physical = *(volatile uint8_t *)AICA_RAM(activate.channel_map +
            local * AFX_CHANNEL_MAP_ENTRY_BYTES);
        uint32_t lane = (!pool && (activate.flags & AFX_FLAG_LANES)) ?
            *(volatile uint8_t *)AICA_RAM(lane_map + local) : 0;
        if (lane >= AFX_MAX_FLOW_CHANNELS) {
            clear_words((volatile uint32_t *)context, sizeof(*context) / 4);
            publish(index, reference, AFX_ERROR, sequence, AFX_BAD_COMMAND, 0, 0, local);
            return;
        }
        if (lane + 1u > lane_count) lane_count = lane + 1u;
        voice_lane_id[physical] = (uint8_t)lane;
        voice_control[physical] = 0;
        voice_lane_gain[physical] = voice_lane_dsp_send[physical] = 255;
        voice_lane_pan[physical] = voice_lane_pitch[physical] = 0;
        voice_flags[physical] = 0;
        voice_held[physical] = 0;
    }
    context->flags = activate.flags;
    context->gain = 255;
    context->lane_count = lane_count;
    context->reference = reference;
    context->state = AFX_RUNNING;
    context->sequence = sequence;
    context->image_base = activate.image_base;
    context->stream_start = activate.stream_offset;
    context->max_lateness = 0;
    context->tempo_period_q8_8 = AFX_TEMPO_NORMAL;
    context->tempo_fraction = 0;
    context->loops = 0;
    song_position[index] = 0;
    if (pool) {
        /* The block: every pool voice free, executor 0 on the first section. */
        context->pc = runtime;
        clear_words((volatile uint32_t *)AICA_RAM(runtime), AFX_SONG_RUNTIME_BYTES(activate.flags & AFX_FLAG_LANES) / 4u);
        volatile afx_song_control_t *control = song_control(context);
        uint32_t voices = activate.required_channels;
        control->free_low = voices >= 32 ? 0xffffffffu : (1u << voices) - 1u;
        control->free_high = voices <= 32 ? 0 : voices == 64 ? 0xffffffffu : (1u << (voices - 32u)) - 1u;
        if (activate.flags & AFX_FLAG_LANES) {
            volatile uint8_t *values = (volatile uint8_t *)AICA_RAM(runtime + AFX_SONG_LANES_OFFSET);
            for (uint32_t lane = 0; lane < 64u; ++lane)
                values[AFX_LANE_GAIN * 64u + lane] = values[AFX_LANE_DSP_SEND * 64u + lane] = 255;
        }
        /* Executor 1 waits with its map empty until the first new-play join. */
        volatile uint32_t *map1 = (volatile uint32_t *)AICA_RAM(runtime + AFX_SONG_MAP_OFFSET(1));
        for (uint32_t word = 0; word < 16u; ++word) map1[word] = 0xffffffffu;
        song_executor(context, 1)->channel_map = runtime + AFX_SONG_MAP_OFFSET(1);
        song_start(context, 0, 0, activate.start_tick, 0);
    } else if (song) song_enter(context, activate.image_base, 0);
    publish(index, reference, AFX_RUNNING, sequence, AFX_OK, stream_position(index, context),
            activate.start_tick, 0);
    STATUS->reserved = 0xa1070000u;
}
static void lifecycle(uint32_t opcode, uint32_t reference, uint32_t sequence) {
    uint32_t index = reference_index(reference);
    if (index >= AFX_MAX_FLOW_SLOTS) return;
    volatile afx_runtime_slot_t *context = &contexts[index];

    if (opcode == AFX_CMD_RECYCLE) {
        volatile afx_observed_t *record = &OBSERVED[index];
        if (context->reference || record->reference != reference ||
            (record->state != AFX_DONE && record->state != AFX_ERROR)) {
            publish(index, reference, AFX_ERROR, sequence, AFX_BUSY, 0, 0, 0);
            return;
        }
        publish(index, 0, AFX_FREE, sequence, AFX_OK, 0, 0, 0);
        return;
    }
    if (context->reference != reference) {
        volatile afx_observed_t *record = &OBSERVED[index];
        /* Natural completion can win the race with a queued STOP or PAUSE. */
        if ((opcode == AFX_CMD_STOP || opcode == AFX_CMD_PAUSE) && !context->reference &&
            record->reference == reference &&
            (record->state == AFX_DONE || record->state == AFX_ERROR)) {
            publish(index, reference, record->state, sequence, record->result,
                    record->position, record->next_deadline, record->detail);
            return;
        }
        publish(index, reference, AFX_ERROR, sequence, AFX_STALE_GENERATION, 0, 0, 0);
        return;
    }
    if (opcode == AFX_CMD_PAUSE) {
        if (context->state != AFX_RUNNING) {
            publish(index, reference, state_word(context), sequence, AFX_BUSY,
                    stream_position(index, context), context->deadline, 0);
            return;
        }
        context->state = AFX_PAUSED;
        context->sequence = sequence;
        keyoff(context);
        if (context->flags & AFX_FLAG_POOL) {
            /* The newest play waits to be resumed; a ringing tail is over. */
            uint32_t newest = song_control(context)->newest;
            song_unmap(context, song_executor(context, 0), 0);
            song_unmap(context, song_executor(context, 1), 0);
            song_executor(context, newest ^ 1u)->state = 0;
        }
        publish(index, reference, state_word(context), sequence, AFX_OK,
                stream_position(index, context), context->deadline, 0);
        return;
    }
    if (opcode == AFX_CMD_STOP) {
        keyoff(context);
            clear_words((volatile uint32_t *)context, sizeof(*context) / 4);
        publish(index, reference, AFX_DONE, sequence, AFX_OK, 0, 0, 0);
        return;
    }
    publish(index, reference, state_word(context), sequence, AFX_UNSUPPORTED,
            stream_position(index, context), context->deadline, 0);
}
static void rebuild(uint32_t reference, uint32_t sequence, uint32_t flags,
                    const uint8_t payload[48]) {
    uint32_t index = reference_index(reference);
    if (index >= AFX_MAX_FLOW_SLOTS) return;
    afx_rebuild_payload_t request;
    for (uint32_t i = 0; i < sizeof(request) / 4; ++i)
        ((uint32_t *)&request)[i] = ((const uint32_t *)payload)[i];
    volatile afx_runtime_slot_t *context = &contexts[index];
    uint32_t stream_end = context->end - context->image_base;
    uint32_t seen_low = 0, seen_high = 0;
    /* A song's position names an order position and an offset in its section. */
    int song = (context->flags & AFX_FLAG_SONG) != 0, pool = (context->flags & AFX_FLAG_POOL) != 0;
    uint32_t order = request.stream_position >> AFX_SONG_POSITION_SHIFT;
    uint32_t offset = request.stream_position & (AFX_SONG_MAX_STREAM - 1u), section_bytes = 0;
    if (song && context->reference == reference && order < SONG_FIELD(context->image_base, order_count)) {
        uint32_t image = context->image_base;
        uint32_t section = *(volatile uint16_t *)AICA_RAM(image + SONG_FIELD(image, order_offset) + order * 2u);
        section_bytes = *(volatile uint32_t *)AICA_RAM(image + SONG_FIELD(image, sections_offset) +
                                               section * AFX_SONG_SECTION_BYTES + 4u);
    }
    if (context->reference != reference || context->state != AFX_PAUSED ||
        (flags & ~AFX_REBUILD_RUN) || request.reserved[0] || request.reserved[1] ||
        request.reserved[2] || request.reserved[3] || request.reserved[4] ||
        request.reserved[5] || request.reserved[6] ||
        request.state_count > (pool ? 0u : context->channels) ||
        !asset_range(request.states_address, request.state_count * sizeof(afx_restore_channel_t)) ||
        (song ? offset >= section_bytes :
                request.stream_position < context->stream_start || request.stream_position >= stream_end)) {
        publish(index, reference, state_word(context), sequence, AFX_BAD_COMMAND,
                stream_position(index, context), context->deadline, 0);
        return;
    }
    for (uint32_t i = 0; i < request.state_count; ++i) {
        const volatile afx_restore_channel_t *source =
            (const volatile afx_restore_channel_t *)AICA_RAM(request.states_address + i * sizeof(*source));
        uint32_t local = source->local_channel;
        uint32_t bit = 1u << (local & 31u);
        uint32_t *seen = local < 32 ? &seen_low : &seen_high;
        if (local >= context->channels || (*seen & bit)) {
            publish(index, reference, state_word(context), sequence, AFX_BAD_COMMAND,
                    stream_position(index, context), context->deadline, local);
            return;
        }
        *seen |= bit;
        /* Validate the whole batch before any voice is stopped or started. */
        uint16_t control = source->fields[AFX_FIELD_CONTROL];
        uint16_t low = source->fields[AFX_FIELD_SAMPLE_LOW];
        afx_result_t result = resolve_sample_address(context, &control, &low);
        if (result) {
            publish(index, reference, state_word(context), sequence, result,
                    stream_position(index, context), context->deadline, local);
            return;
        }
    }
    keyoff(context);
    for (uint32_t i = 0; i < request.state_count; ++i) {
        afx_restore_channel_t state;
        const volatile uint32_t *source = (const volatile uint32_t *)AICA_RAM(request.states_address +
            i * sizeof(state));
        for (uint32_t word = 0; word < sizeof(state) / 4; ++word)
            ((uint32_t *)&state)[word] = source[word];
        afx_result_t result = write_restore(context, &state, (flags & AFX_REBUILD_RUN) != 0);
        if (result) {
            keyoff(context);
            publish(index, reference, state_word(context), sequence, result,
                    stream_position(index, context), context->deadline, state.local_channel);
            return;
        }
    }
    volatile afx_runtime_slot_t *x = context;
    if (pool) {
        /* The newest play resumes on executor 0, with fresh voices. */
        song_unmap(context, song_executor(context, 0), 0);
        song_unmap(context, song_executor(context, 1), 0);
        song_executor(context, 1)->state = 0;
        song_start(context, 0, order, request.next_deadline, 0);
        song_control(context)->newest = 0;
        x = song_executor(context, 0);
    }
    if (song) {
        song_enter(x, context->image_base, order);
        song_position[index] = (uint16_t)order;
        x->pc += offset;
    } else x->pc = context->image_base + request.stream_position;
    x->remaining_wait = request.remaining_wait;
    x->deadline = request.next_deadline;
    context->deadline = request.next_deadline;
    context->state = (flags & AFX_REBUILD_RUN) ? AFX_RUNNING : AFX_PAUSED;
    context->sequence = sequence;
    publish(index, reference, state_word(context), sequence, AFX_OK,
            request.stream_position, request.next_deadline, context->max_lateness);
}
static void patch(uint32_t reference, uint32_t sequence, const uint8_t payload[48]) {
    uint32_t index = reference_index(reference);
    if (index >= AFX_MAX_FLOW_SLOTS) return;
    afx_patch_payload_t request;
    for (uint32_t i = 0; i < sizeof(request) / 4; ++i)
        ((uint32_t *)&request)[i] = ((const uint32_t *)payload)[i];
    volatile afx_runtime_slot_t *context = &contexts[index];
    /* Queued controls can arrive after END; preserve the terminal observation
     * (or a newer generation), as gain and tempo updates already do. */
    if (context->reference != reference) return;
    if ((context->state != AFX_RUNNING && context->state != AFX_PARKED) ||
        (context->flags & AFX_FLAG_POOL) ||
        request.reserved[0] || request.reserved[1] || request.reserved[2] ||
        request.reserved_tail || request.local_channel >= context->channels ||
        (request.mask & ~AFX_PATCH_FIELD_MASK)) {
        publish(index, reference, state_word(context), sequence, AFX_BAD_COMMAND,
                stream_position(index, context), context->deadline, request.local_channel);
        return;
    }
    /* Member by member: a brace initializer of the whole event makes GCC zero
     * the rest with a memset call, which the freestanding firmware lacks. */
    afx_event_t event;
    event.opcode = AFX_OP_PATCH;
    event.channel = request.local_channel;
    event.setup = 0;
    event.wait = event.join = event.bytes = 0;
    event.mask = request.mask;
    event.values = (const uint8_t *)request.values;
    /* A program may patch a channel right after activating its flow, before
     * the flow's first NOTE there: that NOTE keeps what it set. */
    uint32_t physical = 0;
    int unsounded = physical_channel(context, request.local_channel, &physical) &&
                    !(voice_flags[physical] & VOICE_LATCHED);
    afx_result_t result = write_patch(context, &event);
    if (result)
        publish(index, reference, state_word(context), sequence, result,
                stream_position(index, context), context->deadline, request.local_channel);
    else if (unsounded) voice_held[physical] |= request.mask;
}
static void set_instance_gain(uint32_t reference, const uint8_t payload[48]) {
    afx_gain_payload_t request;
    uint32_t index = reference_index(reference);
    for (uint32_t i = 0; i < sizeof(request) / 4; ++i)
        ((uint32_t *)&request)[i] = ((const uint32_t *)payload)[i];
    if (index >= AFX_MAX_FLOW_SLOTS || request.gain > 255 || request.reserved[0] ||
        request.reserved[1] || request.reserved[2] || request.reserved[3] || request.reserved[4] ||
        request.reserved[5] || request.reserved[6] || request.reserved[7] || request.reserved[8] ||
        request.reserved[9] || request.reserved[10]) return;
    volatile afx_runtime_slot_t *context = &contexts[index];
    if (context->reference != reference) return;
    context->gain = (uint8_t)request.gain;
    if (context->flags & AFX_FLAG_POOL)
        song_executor(context, 0)->gain = song_executor(context, 1)->gain = (uint8_t)request.gain;
    for (uint32_t local = 0; local < context->channels; ++local) {
        uint32_t physical;
        if (physical_channel(context, local, &physical) && (voice_flags[physical] & VOICE_LATCHED)) {
            write_projection(physical, 1u << AFX_FIELD_TOTAL_LEVEL, (uint8_t)request.gain);
        }
    }
}
static void set_instance_tempo(uint32_t reference, const uint8_t payload[48]) {
    afx_tempo_payload_t request;
    uint32_t index = reference_index(reference);
    for (uint32_t i = 0; i < sizeof(request) / 4; ++i)
        ((uint32_t *)&request)[i] = ((const uint32_t *)payload)[i];
    if (index >= AFX_MAX_FLOW_SLOTS || request.period_q8_8 < AFX_TEMPO_MIN ||
        request.period_q8_8 > AFX_TEMPO_MAX) return;
    for (uint32_t i = 0; i < 11u; ++i) if (request.reserved[i]) return;
    volatile afx_runtime_slot_t *context = &contexts[index];
    if (context->reference != reference) return;
    context->tempo_period_q8_8 = request.period_q8_8;
    if (context->flags & AFX_FLAG_POOL)
        song_executor(context, 0)->tempo_period_q8_8 =
            song_executor(context, 1)->tempo_period_q8_8 = request.period_q8_8;
}
static void set_lanes(uint32_t reference, uint32_t modifier, const uint8_t payload[48]) {
    afx_lane_payload_t request;
    uint32_t index = reference_index(reference);
    for (uint32_t i = 0; i < sizeof(request) / 4; ++i)
        ((uint32_t *)&request)[i] = ((const uint32_t *)payload)[i];
    if (index >= AFX_MAX_FLOW_SLOTS || modifier >= AFX_LANE_MODIFIER_COUNT ||
        request.reserved[0] || request.reserved[1]) return;
    volatile afx_runtime_slot_t *context = &contexts[index];
    uint32_t lanes = context->lane_count;
    if (context->reference != reference || !(context->flags & AFX_FLAG_LANES) ||
        request.first_lane >= lanes ||
        (lanes - request.first_lane < 32u && request.mask >> (lanes - request.first_lane))) return;
    if (context->flags & AFX_FLAG_POOL) {
        /* Voices that take a lane later start from the song's values. */
        volatile uint8_t *values = (volatile uint8_t *)AICA_RAM(context->pc + AFX_SONG_LANES_OFFSET + modifier * 64u);
        for (uint32_t i = 0; i < 32u && request.first_lane + i < lanes; ++i)
            if (request.mask & (1u << i)) values[request.first_lane + i] = request.values[i];
    }
    for (uint32_t local = 0; local < context->channels; ++local) {
        uint32_t physical;
        if (!physical_channel(context, local, &physical)) continue;
        uint32_t lane = voice_lane_id[physical];
        if (lane < request.first_lane || lane - request.first_lane >= 32u ||
            !(request.mask & (1u << (lane - request.first_lane)))) continue;
        uint8_t value = request.values[lane - request.first_lane];
        if (modifier == AFX_LANE_GAIN) {
            voice_lane_gain[physical] = value;
            if ((voice_flags[physical] & VOICE_LATCHED) && !(voice_flags[physical] & VOICE_RELEASED))
                write_projection(physical, 1u << AFX_FIELD_TOTAL_LEVEL, context->gain);
        } else if (modifier == AFX_LANE_MUTE) {
            if (value) voice_flags[physical] |= VOICE_MUTED;
            else voice_flags[physical] &= ~VOICE_MUTED;
            if (value && (voice_flags[physical] & VOICE_LATCHED) && !(voice_flags[physical] & VOICE_RELEASED)) {
                release_voice(physical);
                voice_flags[physical] |= VOICE_RELEASED;
            }
        } else if (modifier == AFX_LANE_PAN) {
            voice_lane_pan[physical] = (int8_t)value;
            if ((voice_flags[physical] & VOICE_LATCHED) && !(voice_flags[physical] & VOICE_RELEASED))
                write_projection(physical, 1u << AFX_FIELD_DIRECT, context->gain);
        } else if (modifier == AFX_LANE_PITCH) {
            /* A released voice keeps sounding its tail, at the pitch it is set to. */
            voice_lane_pitch[physical] = (int8_t)value;
            if (voice_flags[physical] & VOICE_LATCHED)
                write_projection(physical, 1u << AFX_FIELD_PITCH, context->gain);
        } else {
            voice_lane_dsp_send[physical] = value;
            if ((voice_flags[physical] & VOICE_LATCHED) && !(voice_flags[physical] & VOICE_RELEASED))
                write_projection(physical, 1u << AFX_FIELD_DSP_SEND, context->gain);
        }
    }
}
static void scene_result(uint32_t sequence, uint32_t result) {
    STATUS->dsp_result = result;
    STATUS->dsp_sequence = sequence;
}
static void dsp_control(uint32_t opcode, uint32_t reference, uint32_t sequence,
                        uint32_t flags, const uint8_t payload[48]) {
    afx_dsp_payload_t request;
    for (uint32_t i = 0; i < sizeof(request); ++i) ((uint8_t *)&request)[i] = payload[i];
    uint32_t rbl = 0;
    while (rbl < 3 && request.ring_bytes > ((uint32_t)AFX_DSP_MIN_BYTES << rbl)) ++rbl;
    uint32_t reserved = 0;
    for (uint32_t i = 0; i < 10; ++i) reserved |= request.reserved[i];
    if (reference != AFX_DSP_SCENE_REFERENCE ||
        (flags != 1 && !(opcode == AFX_CMD_DSP_ENABLE && flags == (1u | AFX_DSP_FLAG_HOST_INIT))) || reserved ||
        (opcode == AFX_CMD_DSP_DISABLE && (request.ring_address || request.ring_bytes)) ||
        (!request.ring_bytes && request.ring_address) ||
        (request.ring_bytes &&
         (request.ring_bytes != ((uint32_t)AFX_DSP_MIN_BYTES << rbl) ||
          (request.ring_address & (AFX_DSP_RING_ALIGN - 1u)) ||
          request.ring_address < AICA_ASSET_BASE ||
          !range(request.ring_address, request.ring_bytes, AFX_ASSET_MAX)))) {
        scene_result(sequence, AFX_BAD_COMMAND);
        return;
    }
    if (opcode == AFX_CMD_DSP_ENABLE) {
        dsp_disable();
        dsp_nop();
        if (request.ring_bytes) {
            dsp_delay_base = request.ring_address;
            dsp_delay_bytes = request.ring_bytes;
            if (!(flags & AFX_DSP_FLAG_HOST_INIT)) dsp_clear_delay();
            dsp_write(0x2804u, (rbl << 13) | (dsp_delay_base >> 11));
        }
        dsp_owner = AFX_DSP_SCENE_REFERENCE; /* Prepared silently for program upload. */
    } else if (!dsp_owner || dsp_owner == AFX_DSP_SCENE_REFERENCE) dsp_disable();
    else {
        scene_result(sequence, AFX_BUSY);
        return;
    }
    scene_result(sequence, AFX_OK);
}
static void process_command(const afx_cmd_t *command) {
    STATUS->reserved = 0xa1000000u | command->opcode;
    switch (command->opcode) {
    case AFX_CMD_ACTIVATE:
        activation(command->reference, command->sequence, command->payload);
        break;
    case AFX_CMD_STOP:
    case AFX_CMD_PAUSE:
    case AFX_CMD_RECYCLE:
        lifecycle(command->opcode, command->reference, command->sequence);
        break;
    case AFX_CMD_PATCH:
        patch(command->reference, command->sequence, command->payload);
        break;
    case AFX_CMD_REBUILD:
        rebuild(command->reference, command->sequence, command->flags, command->payload);
        break;
    case AFX_CMD_INSTANCE_GAIN:
        set_instance_gain(command->reference, command->payload);
        break;
    case AFX_CMD_INSTANCE_TEMPO:
        set_instance_tempo(command->reference, command->payload);
        break;
    case AFX_CMD_LANE_SET:
        set_lanes(command->reference, command->flags, command->payload);
        break;
    case AFX_CMD_DSP_ENABLE:
    case AFX_CMD_DSP_DISABLE:
        dsp_control(command->opcode, command->reference, command->sequence,
                    command->flags, command->payload);
        break;
    default: {
        uint32_t index = reference_index(command->reference);
        if (index < AFX_MAX_FLOW_SLOTS)
            publish(index, command->reference, AFX_ERROR, command->sequence,
                    AFX_UNSUPPORTED, 0, 0, command->opcode);
        break;
    }
    }
}
static void drain_commands(void) {
    uint32_t tail = QUEUE->tail;
    uint32_t head = QUEUE->head;
    if (head - tail > AFX_CMD_QUEUE_CAPACITY) {
        STATUS->error = AFX_BAD_FIRMWARE;
        return;
    }
    uint32_t commands = 0;
    while (tail != head && commands++ < AFX_EXECUTOR_MAX_COMMANDS_PER_PASS) {
        afx_cmd_t command;
        volatile uint32_t *source = (volatile uint32_t *)&QUEUE->commands[
            tail & (AFX_CMD_QUEUE_CAPACITY - 1u)];
        uint32_t *target = (uint32_t *)&command;
        for (uint32_t i = 0; i < sizeof(command) / 4; ++i) target[i] = source[i];
        process_command(&command);
        ++tail;
        QUEUE->tail = tail;
        head = QUEUE->head;
        if (head - tail > AFX_CMD_QUEUE_CAPACITY) {
            STATUS->error = AFX_BAD_FIRMWARE;
            return;
        }
    }
}
/* One pass of the main loop. */
#define MAIN_LOOP_PASS() do { \
        drain_commands(); \
        for (uint32_t index = 0; index < AFX_MAX_FLOW_SLOTS; ++index) service(index); \
        STATUS->timer_ticks = *CLOCK; \
        ++STATUS->heartbeat; \
    } while (0)
/* The main loop. The host backend (driver/host/src/firmware.c) defines it
 * empty, so arm_main returns after booting, and runs MAIN_LOOP_PASS itself. */
#ifndef AICA_MAIN_LOOP
#define AICA_MAIN_LOOP() for (;;) MAIN_LOOP_PASS()
#endif
void arm_main(void) {
    AICA_REG_WRITE(0x289c, 0); /* SCIEB */
    AICA_REG_WRITE(0x28b4, 0); /* MCIEB */
    AICA_REG_WRITE(0x28a4, 0x7ff); /* SCIRE */
    AICA_REG_WRITE(0x28bc, 0x7ff); /* MCIRE */
    AICA_REG_WRITE(0x28a8, 0x18); /* SCILV0 */
    AICA_REG_WRITE(0x28ac, 0x50); /* SCILV1 */
    AICA_REG_WRITE(0x28b0, 0x08); /* SCILV2 */
    AICA_REG_WRITE(0x2800, 0); /* master mute */
    if (initialized_data_probe != 0x41584632) { STATUS->error = AFX_BAD_FIRMWARE; return; }

    clear_words((volatile uint32_t *)QUEUE, sizeof(*QUEUE) / 4);
    clear_words((volatile uint32_t *)OBSERVED, AFX_MAX_FLOW_SLOTS * sizeof(*OBSERVED) / 4);
    STATUS->abi = AFX_ABI_VERSION;
    STATUS->layout_id = AFX_LAYOUT_ID;
    dsp_disable();
    STATUS->capabilities = AFX_CAP_BOOTSTRAP | AFX_CAP_LIFECYCLE | AFX_CAP_PLAYBACK | AFX_CAP_DSP | AFX_CAP_DSP_HOST_INIT;
    STATUS->asset_base = AICA_ASSET_BASE;
    STATUS->asset_limit = AFX_ASSET_MAX;
    STATUS->private_end = AICA_PRIVATE_END;
    STATUS->stack_base = AFX_STACK_BASE;
    STATUS->timer_ticks = 0;
    STATUS->error = AFX_OK;
    STATUS->dsp_sequence = STATUS->dsp_result = STATUS->max_lateness = 0;
    STATUS->stack_free[0] = stack_free(AFX_STACK_BASE, AFX_SVC_STACK_TOP);
    STATUS->stack_free[1] = stack_free(AFX_SVC_STACK_TOP, AFX_FIQ_STACK_TOP);
    STATUS->stack_free[2] = stack_free(AFX_FIQ_STACK_TOP, AFX_IRQ_STACK_TOP);
    STATUS->stack_free[3] = stack_free(AFX_IRQ_STACK_TOP, AFX_ABT_STACK_TOP);
    STATUS->stack_free[4] = stack_free(AFX_ABT_STACK_TOP, AFX_UND_STACK_TOP);
    *CLOCK = 0;
    for (uint32_t channel = 0; channel < AFX_AICA_CHANNEL_COUNT; ++channel) {
        AICA_CHANNEL(regs, channel);
        voice_control[channel] = AFX_KEYON_EXECUTE;
        voice_base_mix[channel] = voice_base_direct[channel] =
            voice_base_dsp_send[channel] = 0;
        voice_flags[channel] = 0;
        voice_lane_gain[channel] = voice_lane_dsp_send[channel] = 255;
        voice_lane_pitch[channel] = 0;
        voice_lane_id[channel] = 0;
        voice_lane_pan[channel] = 0;
        AICA_CHANNEL_SET(regs, AFX_FIELD_CONTROL, AFX_KEYON_EXECUTE);
        for (uint32_t field = 1; field < AFX_FIELD_COUNT; ++field) AICA_CHANNEL_SET(regs, field, 0);
        AICA_CHANNEL_SET(regs, AFX_FIELD_ENV_DR, 0x1f);
    }
    AICA_REG_WRITE(0x2800, 0x0f);
    AICA_REG_WRITE(0x2890, AFX_TIMER_RELOAD);
    AICA_REG_WRITE(0x289c, 0x40); /* Timer A only */
    STATUS->magic = AFX_STATUS_MAGIC; /* publish only after all shared state is ready */
    /* Once, so afx_init can see the interrupt arrive before relying on it. */
    AICA_REG_WRITE(AFX_AICA_MCIPD, AFX_AICA_MCI_SCPU);
    arm_fiq_enable();
    AICA_MAIN_LOOP();
}
