#include <aicaflow/protocol.h>
#include <aicaflow/codec.h>
#include "sample_address.h"

/* The timer FIQ only advances AFX_AICA_TIMER_TICK_ADDR. Stream work stays in
 * this normal ARM context, which keeps the explicitly reserved FIQ stack free
 * of decoder and register-write pressure. */
static volatile afx_runtime_slot_t contexts[AFX_MAX_FLOW_SLOTS];
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
enum { VOICE_MUTED = 1u, VOICE_LATCHED = 2u, VOICE_RELEASED = 4u };
static uint8_t voice_flags[AFX_AICA_CHANNEL_COUNT];
/* The AICA channel aperture is write-only for our purposes. These retain the
 * authored state before instance/lane projection and the words needed by KEYOFF. */
static uint16_t voice_control[AFX_AICA_CHANNEL_COUNT];
static uint16_t voice_base_mix[AFX_AICA_CHANNEL_COUNT];
static uint16_t voice_base_direct[AFX_AICA_CHANNEL_COUNT];
static uint16_t voice_base_dsp_send[AFX_AICA_CHANNEL_COUNT];
static uint32_t dsp_owner;
static uint32_t dsp_delay_base, dsp_delay_bytes;
extern uint8_t __asset_base[], __private_end[];
extern void arm_fiq_enable(void);

#define STATUS ((volatile afx_status_t *)AFX_STATUS_ADDR)
#define QUEUE ((volatile afx_cmd_queue_t *)AFX_QUEUE_ADDR)
#define OBSERVED ((volatile afx_observed_t *)AFX_OBSERVED_ADDR)
#define CLOCK ((volatile uint32_t *)AFX_AICA_TIMER_TICK_ADDR)

/* Original bus-0 DSP programs; no extracted commercial payloads. */
static void dsp_write(uint32_t offset, uint16_t value) {
    /* AFX stores the meaningful low 16 bits; AICA's register aperture is
     * 32-bit word addressed, as are the per-channel registers below. */
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + offset) = value;
}
static void dsp_returns(int enabled) {
    dsp_write(0x2000u, enabled ? 0x0f1fu : 0);
    dsp_write(0x2004u, enabled ? 0x0f0fu : 0);
}
static void dsp_clear_delay(void) {
    volatile uint32_t *delay = (volatile uint32_t *)dsp_delay_base;
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
    while (current < high && *(volatile uint32_t *)current == AFX_STACK_PATTERN)
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
        uint32_t physical = *(volatile uint8_t *)(address + i * AFX_CHANNEL_MAP_ENTRY_BYTES);
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
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + physical * AFX_AICA_CHANNEL_REG_STRIDE) =
        voice_control[physical];
}
static void keyoff(const volatile afx_runtime_slot_t *context) {
    for (uint32_t local = 0; local < context->channels; ++local) {
        uint32_t physical;
        if (!physical_channel(context, local, &physical)) continue;
        /* Retirement/pause must not leave an authored release tail playing. */
        volatile uint32_t *regs = (volatile uint32_t *)(AFX_AICA_REG_BASE +
            physical * AFX_AICA_CHANNEL_REG_STRIDE);
        regs[AFX_FIELD_TOTAL_LEVEL] = 0xff00u | (voice_base_mix[physical] & 0xffu);
        regs[AFX_FIELD_ENV_DR] = 31;
        release_voice(physical);
        voice_flags[physical] &= VOICE_MUTED;
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
    return (uint16_t)((attenuation << 8) | (total_level & 0x0fu));
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
static uint16_t projected_dsp_send(uint32_t physical) {
    if (voice_flags[physical] & VOICE_RELEASED) return voice_base_dsp_send[physical];
    uint32_t level = (voice_base_dsp_send[physical] >> 4) & 15u;
    level = multiply_gain((uint8_t)level, voice_lane_dsp_send[physical]);
    return (uint16_t)((voice_base_dsp_send[physical] & ~0xf0u) | (level << 4));
}
static void write_projection(uint32_t physical, uint32_t fields, uint8_t instance_gain) {
    volatile uint32_t *regs = (volatile uint32_t *)(AFX_AICA_REG_BASE +
        physical * AFX_AICA_CHANNEL_REG_STRIDE);
    if (fields & (1u << AFX_FIELD_TOTAL_LEVEL))
        regs[AFX_FIELD_TOTAL_LEVEL] = projected_mix(physical, instance_gain);
    if (fields & (1u << AFX_FIELD_DIRECT)) regs[AFX_FIELD_DIRECT] = projected_direct(physical);
    if (fields & (1u << AFX_FIELD_DSP_SEND))
        regs[AFX_FIELD_DSP_SEND] = projected_dsp_send(physical);
}
static int physical_channel(const volatile afx_runtime_slot_t *context,
                            uint32_t local, uint32_t *out) {
    if (local >= context->channels) return 0;
    uint32_t physical = *(volatile uint8_t *)(context->channel_map +
                                                local * AFX_CHANNEL_MAP_ENTRY_BYTES);
    if (physical >= AFX_AICA_CHANNEL_COUNT) return 0;
    *out = physical;
    return 1;
}
static void complete(uint32_t index, volatile afx_runtime_slot_t *context,
                     uint32_t state, uint32_t result, uint32_t detail) {
    uint32_t reference = context->reference;
    uint32_t sequence = context->sequence;
    uint32_t position = context->pc - context->image_base;
    uint32_t deadline = context->deadline;
    uint32_t max_lateness = context->max_lateness;
    keyoff(context);
    clear_words((volatile uint32_t *)context, sizeof(*context) / 4);
    if (result == AFX_OK) detail = max_lateness;
    publish(index, reference, state, sequence, result, position, deadline, detail);
}
static uint32_t stream_position(const volatile afx_runtime_slot_t *context) {
    return context->pc - context->image_base;
}
/* Install a complete NOTE/RESTORE state; key-on is always the final write. */
static afx_result_t install_voice(const volatile afx_runtime_slot_t *context,
                                  uint32_t physical, uint16_t words[AFX_FIELD_COUNT], int run) {
    afx_result_t result = resolve_sample_address(context, &words[AFX_FIELD_CONTROL],
                                                &words[AFX_FIELD_SAMPLE_LOW]);
    if (result) return result;
    voice_flags[physical] = (voice_flags[physical] & VOICE_MUTED) | VOICE_LATCHED;
    voice_base_mix[physical] = words[AFX_FIELD_TOTAL_LEVEL];
    voice_base_direct[physical] = words[AFX_FIELD_DIRECT];
    voice_base_dsp_send[physical] = words[AFX_FIELD_DSP_SEND];
    words[AFX_FIELD_TOTAL_LEVEL] = projected_mix(physical, context->gain);
    words[AFX_FIELD_DIRECT] = projected_direct(physical);
    words[AFX_FIELD_DSP_SEND] = projected_dsp_send(physical);
    volatile uint32_t *regs = (volatile uint32_t *)(AFX_AICA_REG_BASE +
        physical * AFX_AICA_CHANNEL_REG_STRIDE);
    uint16_t control = words[AFX_FIELD_CONTROL] & ~(AFX_KEYON | AFX_KEYON_EXECUTE);
    regs[AFX_FIELD_CONTROL] = control;
    for (uint32_t field = 1; field < AFX_FIELD_COUNT; ++field) regs[field] = words[field];
    if (run) control |= AFX_KEYON | AFX_KEYON_EXECUTE;
    voice_control[physical] = control;
    if (run) regs[AFX_FIELD_CONTROL] = control;
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
        (const uint8_t *)(context->setups + event->setup * AFX_SETUP_BYTES), event->mask,
        event->values, afx_field_value_bytes(event->mask));
    if (result) return result;
    return install_voice(context, physical, state, 1);
}

static afx_result_t write_patch(const volatile afx_runtime_slot_t *context,
                                const afx_event_t *event) {
    uint32_t physical;
    if (!physical_channel(context, event->channel, &physical)) return AFX_BAD_COMMAND;
    volatile uint32_t *regs = (volatile uint32_t *)(AFX_AICA_REG_BASE +
        physical * AFX_AICA_CHANNEL_REG_STRIDE);
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
        } else regs[field] = field_value;
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

static void service(uint32_t index) {
    volatile afx_runtime_slot_t *context = &contexts[index];
    if (context->state != AFX_RUNNING) return;
    uint32_t events = 0;
    while (events < AFX_EXECUTOR_MAX_EVENTS_PER_PASS) {
        uint32_t now = *CLOCK;
        if (!tick_due(now, context->deadline)) return;
        record_lateness(context, now);
        /* Split a legal very long WAIT32 into rollover-safe deadline steps. */
        if (context->remaining_wait) {
            schedule_wait(context, context->remaining_wait);
            continue;
        }
        if (context->pc >= context->end) {
            complete(index, context, AFX_ERROR, AFX_BAD_BOUNDS, now - context->deadline);
            return;
        }
        afx_event_t event;
        afx_result_t result = afx_decode_event((const uint8_t *)context->pc,
                                               context->end - context->pc, &event);
        if (result) {
            complete(index, context, AFX_ERROR, result, now - context->deadline);
            return;
        }
        context->pc += event.bytes;
        ++events;
        switch (event.opcode) {
        case AFX_OP_WAIT8:
        case AFX_OP_WAIT16:
        case AFX_OP_WAIT32: {
            schedule_wait(context, event.wait);
            break;
        }
        case AFX_OP_NOTE:
            result = write_note(context, &event);
            break;
        case AFX_OP_PATCH:
            result = write_patch(context, &event);
            break;
        case AFX_OP_KEYOFF: {
            uint32_t physical;
            if (!physical_channel(context, event.channel, &physical)) result = AFX_BAD_COMMAND;
            else {
                release_voice(physical);
                /* KEYOFF may begin an audible release; later authored TOTAL_LEVEL
                 * automation retains the NOTE's instance/lane projection until a
                 * new NOTE replaces it or the instance is explicitly retired. */
            }
            break;
        }
        case AFX_OP_END:
            complete(index, context, AFX_DONE, AFX_OK, now - context->deadline);
            return;
        case AFX_OP_PARK:
            if (!(context->flags & AFX_FLAG_CONTROLLED)) {
                complete(index, context, AFX_ERROR, AFX_BAD_COMMAND, now - context->deadline);
                return;
            }
            context->state = AFX_PARKED;
            publish(index, context->reference, AFX_PARKED, context->sequence, AFX_OK,
                    stream_position(context), context->deadline, now - context->deadline);
            return;
        default:
            result = AFX_BAD_COMMAND;
            break;
        }
        if (result) {
            complete(index, context, AFX_ERROR, result, now - context->deadline);
            return;
        }
    }
    publish(index, context->reference, AFX_RUNNING, context->sequence, AFX_OK,
            stream_position(context), context->deadline, context->max_lateness);
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
    if (contexts[index].reference || activate.reserved ||
        (!activate.bank_bytes && bank_base) ||
        (activate.bank_bytes && (bank_base < (uint32_t)(uintptr_t)__asset_base ||
         !asset_range(bank_base, activate.bank_bytes))) ||
        !activate.required_channels || activate.required_channels > AFX_MAX_FLOW_CHANNELS ||
        !asset_range(activate.image_base, activate.image_size) ||
        !range(activate.stream_offset, activate.stream_size, activate.image_size) ||
        !activate.stream_size || (activate.setups_offset & 1u) ||
        activate.setup_count > 65536u ||
        !range(activate.setups_offset, activate.setup_count * AFX_SETUP_BYTES, activate.image_size) ||
        ((activate.flags & AFX_FLAG_LANES) &&
         (activate.setups_offset + activate.setup_count * AFX_SETUP_BYTES > activate.stream_offset ||
          activate.required_channels > activate.stream_offset -
              (activate.setups_offset + activate.setup_count * AFX_SETUP_BYTES))) ||
        !map_valid(activate.channel_map, activate.required_channels) ||
        (activate.flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_MUSIC | AFX_FLAG_MUSIC_CHORUS |
                            AFX_FLAG_LANES))) {
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
    for (uint32_t local = 0; local < activate.required_channels; ++local) {
        uint32_t physical = *(volatile uint8_t *)(activate.channel_map +
            local * AFX_CHANNEL_MAP_ENTRY_BYTES);
        uint32_t lane = (activate.flags & AFX_FLAG_LANES) ? *(volatile uint8_t *)(lane_map + local) : 0;
        if (lane >= AFX_MAX_FLOW_CHANNELS) {
            clear_words((volatile uint32_t *)context, sizeof(*context) / 4);
            publish(index, reference, AFX_ERROR, sequence, AFX_BAD_COMMAND, 0, 0, local);
            return;
        }
        if (lane + 1u > lane_count) lane_count = lane + 1u;
        voice_lane_id[physical] = (uint8_t)lane;
        voice_control[physical] = 0;
        voice_lane_gain[physical] = voice_lane_dsp_send[physical] = 255;
        voice_lane_pan[physical] = 0;
        voice_flags[physical] = 0;
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
    publish(index, reference, AFX_RUNNING, sequence, AFX_OK, activate.stream_offset,
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
        /* Natural completion can win the race with a queued STOP. */
        if (opcode == AFX_CMD_STOP && !context->reference && record->reference == reference &&
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
            publish(index, reference, context->state, sequence, AFX_BUSY,
                    stream_position(context), context->deadline, 0);
            return;
        }
        context->state = AFX_PAUSED;
        context->sequence = sequence;
        keyoff(context);
        publish(index, reference, AFX_PAUSED, sequence, AFX_OK,
                stream_position(context), context->deadline, 0);
        return;
    }
    if (opcode == AFX_CMD_STOP) {
        keyoff(context);
            clear_words((volatile uint32_t *)context, sizeof(*context) / 4);
        publish(index, reference, AFX_DONE, sequence, AFX_OK, 0, 0, 0);
        return;
    }
    publish(index, reference, context->state, sequence, AFX_UNSUPPORTED,
            stream_position(context), context->deadline, 0);
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
    if (context->reference != reference || context->state != AFX_PAUSED ||
        (flags & ~AFX_REBUILD_RUN) || request.reserved[0] || request.reserved[1] ||
        request.reserved[2] || request.reserved[3] || request.reserved[4] ||
        request.reserved[5] || request.reserved[6] ||
        request.state_count > context->channels ||
        !asset_range(request.states_address, request.state_count * sizeof(afx_restore_channel_t)) ||
        request.stream_position < context->stream_start ||
        request.stream_position >= stream_end) {
        publish(index, reference, context->state, sequence, AFX_BAD_COMMAND,
                stream_position(context), context->deadline, 0);
        return;
    }
    for (uint32_t i = 0; i < request.state_count; ++i) {
        const volatile afx_restore_channel_t *source =
            (const volatile afx_restore_channel_t *)(request.states_address + i * sizeof(*source));
        uint32_t local = source->local_channel;
        uint32_t bit = 1u << (local & 31u);
        uint32_t *seen = local < 32 ? &seen_low : &seen_high;
        if (local >= context->channels || (*seen & bit)) {
            publish(index, reference, context->state, sequence, AFX_BAD_COMMAND,
                    stream_position(context), context->deadline, local);
            return;
        }
        *seen |= bit;
        /* Validate the whole batch before any voice is stopped or started. */
        uint16_t control = source->fields[AFX_FIELD_CONTROL];
        uint16_t low = source->fields[AFX_FIELD_SAMPLE_LOW];
        afx_result_t result = resolve_sample_address(context, &control, &low);
        if (result) {
            publish(index, reference, context->state, sequence, result,
                    stream_position(context), context->deadline, local);
            return;
        }
    }
    keyoff(context);
    for (uint32_t i = 0; i < request.state_count; ++i) {
        afx_restore_channel_t state;
        const volatile uint32_t *source = (const volatile uint32_t *)(request.states_address +
            i * sizeof(state));
        for (uint32_t word = 0; word < sizeof(state) / 4; ++word)
            ((uint32_t *)&state)[word] = source[word];
        afx_result_t result = write_restore(context, &state, (flags & AFX_REBUILD_RUN) != 0);
        if (result) {
            keyoff(context);
            publish(index, reference, context->state, sequence, result,
                    stream_position(context), context->deadline, state.local_channel);
            return;
        }
    }
    context->pc = context->image_base + request.stream_position;
    context->remaining_wait = request.remaining_wait;
    context->deadline = request.next_deadline;
    context->state = (flags & AFX_REBUILD_RUN) ? AFX_RUNNING : AFX_PAUSED;
    context->sequence = sequence;
    publish(index, reference, context->state, sequence, AFX_OK,
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
        request.reserved[0] || request.reserved[1] || request.reserved[2] ||
        request.reserved_tail || request.local_channel >= context->channels ||
        (request.mask & ~AFX_PATCH_FIELD_MASK)) {
        publish(index, reference, context->state, sequence, AFX_BAD_COMMAND,
                stream_position(context), context->deadline, request.local_channel);
        return;
    }
    afx_event_t event = { .opcode = AFX_OP_PATCH, .channel = request.local_channel,
                          .mask = request.mask, .values = (const uint8_t *)request.values };
    afx_result_t result = write_patch(context, &event);
    if (result)
        publish(index, reference, context->state, sequence, result,
                stream_position(context), context->deadline, request.local_channel);
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
    if (context->reference == reference) context->tempo_period_q8_8 = request.period_q8_8;
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
          request.ring_address < (uint32_t)(uintptr_t)__asset_base ||
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
void arm_main(void) {
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x289c) = 0; /* SCIEB */
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x28b4) = 0; /* MCIEB */
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x28a4) = 0x7ff; /* SCIRE */
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x28bc) = 0x7ff; /* MCIRE */
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x28a8) = 0x18; /* SCILV0 */
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x28ac) = 0x50; /* SCILV1 */
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x28b0) = 0x08; /* SCILV2 */
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x2800) = 0; /* master mute */
    if (initialized_data_probe != 0x41584632) { STATUS->error = AFX_BAD_FIRMWARE; return; }

    clear_words((volatile uint32_t *)QUEUE, sizeof(*QUEUE) / 4);
    clear_words((volatile uint32_t *)OBSERVED, AFX_MAX_FLOW_SLOTS * sizeof(*OBSERVED) / 4);
    STATUS->abi = AFX_ABI_VERSION;
    STATUS->layout_id = AFX_LAYOUT_ID;
    dsp_disable();
    STATUS->capabilities = AFX_CAP_BOOTSTRAP | AFX_CAP_LIFECYCLE | AFX_CAP_PLAYBACK | AFX_CAP_DSP | AFX_CAP_DSP_HOST_INIT;
    STATUS->asset_base = (uint32_t)__asset_base;
    STATUS->asset_limit = AFX_ASSET_MAX;
    STATUS->private_end = (uint32_t)__private_end;
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
        volatile uint32_t *regs = (volatile uint32_t *)(AFX_AICA_REG_BASE +
            channel * AFX_AICA_CHANNEL_REG_STRIDE);
        voice_control[channel] = AFX_KEYON_EXECUTE;
        voice_base_mix[channel] = voice_base_direct[channel] =
            voice_base_dsp_send[channel] = 0;
        voice_flags[channel] = 0;
        voice_lane_gain[channel] = voice_lane_dsp_send[channel] = 255;
        voice_lane_id[channel] = 0;
        voice_lane_pan[channel] = 0;
        regs[AFX_FIELD_CONTROL] = AFX_KEYON_EXECUTE;
        for (uint32_t field = 1; field < AFX_FIELD_COUNT; ++field) regs[field] = 0;
        regs[AFX_FIELD_ENV_DR] = 0x1f;
    }
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x2800) = 0x0f;
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x2890) = AFX_TIMER_RELOAD;
    *(volatile uint32_t *)(AFX_AICA_REG_BASE + 0x289c) = 0x40; /* Timer A only */
    STATUS->magic = AFX_STATUS_MAGIC; /* publish only after all shared state is ready */
    arm_fiq_enable();
    for (;;) {
        drain_commands();
        for (uint32_t index = 0; index < AFX_MAX_FLOW_SLOTS; ++index) service(index);
        STATUS->timer_ticks = *CLOCK;
        ++STATUS->heartbeat;
    }
}
