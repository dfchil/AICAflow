#include "afx_compile_c.h"

#include <aicaflow/codec.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { AFB_HEADER = AFX_BANK_HEADER_BYTES, AFC_HEADER = AFX_SEEK_HEADER_BYTES,
       AFX_HEADER = 80, RELOCATION_BYTES = 12,
       SINE_FRAMES = 128, VISUAL_HEADER = 12, VISUAL_BANDS = 32, VISUAL_RATE = 60,
       NOTE_WRITES = 19 };

typedef struct { uint32_t tick; uint8_t kind, channel, key, velocity; uint16_t setup, mix; int16_t pitch_bend; } event_t;
typedef struct {
    uint16_t source_setup, setup;
    uint16_t words[AFX_FIELD_COUNT];
    uint32_t uses;
    uint8_t retained;
} setup_variant_t;

static uint32_t hash32(const uint8_t *p, uint32_t bytes) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 16777619u;
    return h ? h : 1;
}

static uint32_t hash32_alt(const uint8_t *p, uint32_t bytes) {
    uint32_t h = 2166136261u ^ 0x9e3779b9u;
    for (uint32_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 16777619u;
    return h ? h : 1;
}

static int compare_note(const void *left, const void *right) {
    const afx_c_note_t *a = left, *b = right;
    if (a->start_tick != b->start_tick) return a->start_tick < b->start_tick ? -1 : 1;
    /* Preserve the score's order through voice allocation.  Sorting a chord
       by its end time makes the generated channel identity depend on release
       tails, which in turn detaches AFP note lanes from their intended tone. */
    if (a->source_track != b->source_track) return a->source_track < b->source_track ? -1 : 1;
    if (a->source_order != b->source_order) return a->source_order < b->source_order ? -1 : 1;
    if (a->source_id != b->source_id) return a->source_id < b->source_id ? -1 : 1;
    if (a->end_tick != b->end_tick) return a->end_tick < b->end_tick ? -1 : 1;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->velocity < b->velocity ? -1 : a->velocity > b->velocity;
}

static int compare_event(const void *left, const void *right) {
    const event_t *a = left, *b = right;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    if (a->kind != b->kind) return (int)a->kind - (int)b->kind; /* KEYOFF first. */
    return (int)a->channel - (int)b->channel;
}

/* AICA can start at most nine fully configured voices in one 1 ms executor
 * pass (9 * 19 register writes). Preserve every note's duration, but spread
 * a denser MIDI chord across the next few milliseconds before it reaches the
 * ARM7. This is below the visualizer's 60 Hz resolution and avoids an
 * otherwise unplayable flow. */
int afx_c_schedule_notes(afx_c_note_t *notes, uint32_t count, uint8_t cluster_limit) {
    uint32_t final_tick = 0;
    if (!notes || !count || !cluster_limit || cluster_limit > AFX_EXECUTION_BUDGET_COMMANDS) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t keyoff = notes[i].release_tick ? notes[i].release_tick : notes[i].end_tick;
        if (!notes[i].velocity || keyoff <= notes[i].start_tick || notes[i].end_tick < keyoff)
            return -1;
        if (notes[i].end_tick > final_tick) final_tick = notes[i].end_tick;
    }
    if (final_tick > UINT32_MAX - count - 1u) return -1;
    uint32_t slots = final_tick + count + 1u;
    uint8_t *commands = calloc(slots, 1), *writes = calloc(slots, 1);
    if (!commands || !writes) { free(commands); free(writes); return -1; }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t duration = notes[i].end_tick - notes[i].start_tick;
        uint32_t keyoff_offset = (notes[i].release_tick ? notes[i].release_tick : notes[i].end_tick) - notes[i].start_tick;
        uint32_t tick = notes[i].start_tick;
        while (tick <= final_tick + count - duration) {
            uint32_t end = tick + duration, keyoff = tick + keyoff_offset;
            if (commands[tick] < cluster_limit &&
                writes[tick] <= AFX_EXECUTION_BUDGET_WRITES - NOTE_WRITES &&
                commands[keyoff] < AFX_EXECUTION_BUDGET_COMMANDS &&
                writes[keyoff] < AFX_EXECUTION_BUDGET_WRITES) {
                ++commands[tick]; writes[tick] += NOTE_WRITES;
                ++commands[keyoff]; ++writes[keyoff];
                notes[i].start_tick = tick;
                notes[i].end_tick = end;
                notes[i].release_tick = keyoff;
                break;
            }
            ++tick;
        }
        if (tick > final_tick + count - duration) { free(commands); free(writes); return -1; }
    }
    free(commands); free(writes);
    return 0;
}

int afx_c_assign_channels(const afx_c_note_t *notes, uint32_t count,
                          uint8_t *out_channels, uint32_t *out_channel_count) {
    uint32_t ends[64] = {0}, channels = 0;
    if (!notes || !count || !out_channels || !out_channel_count) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t keyoff = notes[i].release_tick ? notes[i].release_tick : notes[i].end_tick;
        if (!notes[i].velocity || keyoff <= notes[i].start_tick || notes[i].end_tick < keyoff) return -1;
        uint32_t channel = 0;
        while (channel < channels && ends[channel] > notes[i].start_tick) ++channel;
        if (channel == channels) { if (channels == 64) return -1; ++channels; }
        ends[channel] = notes[i].end_tick;
        out_channels[i] = (uint8_t)channel;
    }
    *out_channel_count = channels;
    return 0;
}

static uint16_t pitch(uint8_t key, uint8_t root_key, int tuning_cents, uint32_t sample_rate) {
    /* AICA reads samples at its 44.1 kHz base rate. A downsampled source must
     * therefore lower FNS by log2(source_rate / 44100), in addition to the
     * SF2 root-key/pitch-correction and MIDI bend. */
    if (!sample_rate) sample_rate = 44100;
    /* Keep the rate correction on the same whole-cent grid as the former
       authoring path.  Carrying the fractional value looks more precise, but
       it changes an occasional AICA FNS unit and prevents reproducible C/Python
       source lowering. */
    int rate_cents = (int)lround(1200.0 * log2((double)sample_rate / 44100.0));
    double ratio = pow(2.0, (((int)key - root_key) * 100.0 + tuning_cents + rate_cents) / 1200.0);
    int octave = (int)floor(log2(ratio));
    int fraction = (int)(1024.0 * (ratio / pow(2.0, octave) - 1.0));
    if (octave < -8) octave = -8;
    if (octave > 7) octave = 7;
    if (fraction < 0) fraction = 0;
    if (fraction > 1023) fraction = 1023;
    return (uint16_t)(((octave & 15) << 11) | fraction);
}

static uint16_t level(uint8_t velocity) {
    uint32_t attenuation = (127u - velocity) * 2u;
    return (uint16_t)((attenuation > 255 ? 255 : attenuation) << 8 | 0x24u);
}

static uint32_t encode_wait(uint8_t *out, uint32_t ticks) {
    if (!ticks) return 0;
    if (ticks <= 255) { out[0] = AFX_OP_WAIT8; out[1] = (uint8_t)ticks; return 2; }
    if (ticks <= 65535) { out[0] = AFX_OP_WAIT16; afx_write16(out + 1, (uint16_t)ticks); return 3; }
    out[0] = AFX_OP_WAIT32; afx_write32(out + 1, ticks); return 5;
}

static uint32_t align32(uint32_t value) { return (value + 31u) & ~31u; }

static int same_sample(const afx_c_sample_t *a, const afx_c_sample_t *b) {
    return a->data == b->data && a->bytes == b->bytes && a->frames == b->frames &&
           a->format == b->format && a->root_key == b->root_key && a->loop == b->loop &&
           a->loop_start == b->loop_start && a->loop_end == b->loop_end &&
           a->tuning_cents == b->tuning_cents && a->sample_rate == b->sample_rate;
}

/* Pitch and MIX are supplied by every generated NOTE.  The remaining setup
 * words can be shared greedily and, when necessary, overridden in that NOTE.
 * This is the same trade-off as the VGM compiler: a 36-byte setup only wins
 * when its saved NOTE bytes pay for it. */
static uint16_t zone_word(const afx_c_zone_t *zone, uint32_t field) {
    uint16_t value = 0;
    if (field == AFX_FIELD_ENV_AD || field == AFX_FIELD_ENV_DR) value = 0x001f;
    else if (field == AFX_FIELD_DSP_SEND) value = zone->dsp_send;
    else if (field == AFX_FIELD_DIRECT) value = 0x0f10;
    else if (field == AFX_FIELD_MIX) value = 0x0024;
    else if (field >= AFX_FIELD_FILTER_LEVEL0 && field <= AFX_FIELD_FILTER_LEVEL4) value = 0x1fffu;
    if (zone->setup_mask & (1u << field)) value = zone->setup[field];
    return value;
}

static int compare_variant(const uint16_t a[AFX_FIELD_COUNT], const uint16_t b[AFX_FIELD_COUNT]) {
    for (uint32_t field = AFX_FIELD_ENV_AD; field < AFX_FIELD_COUNT; ++field) {
        if (field == AFX_FIELD_PITCH || field == AFX_FIELD_MIX) continue;
        uint16_t av = a[field], bv = b[field];
        if (av != bv) return av < bv ? -1 : 1;
    }
    return 0;
}

static uint32_t variant_differences(const uint16_t a[AFX_FIELD_COUNT], const uint16_t b[AFX_FIELD_COUNT]) {
    uint32_t differences = 0;
    for (uint32_t field = AFX_FIELD_ENV_AD; field < AFX_FIELD_COUNT; ++field)
        if (field != AFX_FIELD_PITCH && field != AFX_FIELD_MIX && a[field] != b[field])
            ++differences;
    return differences;
}

static int same_variant(const setup_variant_t *a, const setup_variant_t *b,
                        const afx_c_zone_t *zones) {
    return same_sample(&zones[a->source_setup].sample, &zones[b->source_setup].sample) &&
           !compare_variant(a->words, b->words);
}

int afx_c_optimize_events(const afx_c_event_t *input, uint32_t event_count,
                          const afx_c_zone_t *zones, uint32_t zone_count,
                          afx_c_event_t **out_events, afx_c_zone_t **out_zones,
                          uint32_t *out_zone_count) {
    afx_c_event_t *events = NULL;
    afx_c_zone_t *templates = NULL;
    setup_variant_t *variants = NULL;
    uint32_t variant_count = 0, template_count = 0;
    if (!input || !event_count || !zones || !zone_count || zone_count > UINT16_MAX ||
        !out_events || !out_zones || !out_zone_count) return -1;
    events = malloc(event_count * sizeof(*events));
    templates = malloc(zone_count * sizeof(*templates));
    variants = calloc(event_count, sizeof(*variants));
    if (!events || !templates || !variants) goto failed;
    for (uint32_t event = 0; event < event_count; ++event) {
        const afx_c_event_t *source = input + event;
        if (source->opcode != AFX_OP_NOTE) continue;
        if (source->setup >= zone_count || (source->mask & AFX_NOTE_PL_MASK) != AFX_NOTE_PL_MASK ||
            source->mask & ((1u << AFX_FIELD_ENV_AD) - 1u) || source->mask & ~AFX_FIELD_MASK) goto failed;
        setup_variant_t candidate = {.source_setup = source->setup, .setup = UINT16_MAX, .uses = 1};
        for (uint32_t field = AFX_FIELD_ENV_AD; field < AFX_FIELD_COUNT; ++field)
            candidate.words[field] = source->mask & (1u << field) ? source->fields[field] : zone_word(zones + source->setup, field);
        uint32_t found = 0;
        while (found < variant_count && !same_variant(variants + found, &candidate, zones)) ++found;
        if (found == variant_count) variants[variant_count++] = candidate;
        else ++variants[found].uses;
    }
    if (!variant_count) goto failed;
    for (uint32_t first = 0; first < variant_count; ++first) {
        if (variants[first].setup != UINT16_MAX) continue;
        for (;;) {
            uint32_t choice = UINT32_MAX, reference = UINT32_MAX, cost = UINT32_MAX;
            for (uint32_t i = first; i < variant_count; ++i) {
                if (variants[i].setup != UINT16_MAX ||
                    !same_sample(&zones[variants[first].source_setup].sample, &zones[variants[i].source_setup].sample)) continue;
                if (choice == UINT32_MAX || variants[i].uses > variants[choice].uses ||
                    (variants[i].uses == variants[choice].uses &&
                     compare_variant(variants[i].words, variants[choice].words) < 0)) choice = i;
            }
            if (choice == UINT32_MAX) break;
            for (uint32_t i = first; i < variant_count; ++i) {
                if (!variants[i].retained ||
                    !same_sample(&zones[variants[first].source_setup].sample, &zones[variants[i].source_setup].sample)) continue;
                uint32_t candidate_cost = variants[choice].uses *
                    (4u + 2u * variant_differences(variants[choice].words, variants[i].words));
                if (candidate_cost < cost || (candidate_cost == cost && i < reference)) {
                    cost = candidate_cost; reference = i;
                }
            }
            if (reference != UINT32_MAX && cost < AFX_SETUP_BYTES) variants[choice].setup = variants[reference].setup;
            else {
                if (template_count == UINT16_MAX) goto failed;
                templates[template_count] = zones[variants[choice].source_setup];
                for (uint32_t field = AFX_FIELD_ENV_AD; field < AFX_FIELD_COUNT; ++field) {
                    if (field == AFX_FIELD_PITCH || field == AFX_FIELD_MIX) continue;
                    templates[template_count].setup_mask |= 1u << field;
                    templates[template_count].setup[field] = variants[choice].words[field];
                }
                templates[template_count].dsp_send = (uint8_t)variants[choice].words[AFX_FIELD_DSP_SEND];
                variants[choice].setup = (uint16_t)template_count++;
                variants[choice].retained = 1;
            }
        }
    }
    memcpy(events, input, event_count * sizeof(*events));
    for (uint32_t event = 0; event < event_count; ++event) if (events[event].opcode == AFX_OP_NOTE) {
        setup_variant_t candidate = {.source_setup = events[event].setup};
        for (uint32_t field = AFX_FIELD_ENV_AD; field < AFX_FIELD_COUNT; ++field)
            candidate.words[field] = events[event].mask & (1u << field) ? events[event].fields[field] : zone_word(zones + events[event].setup, field);
        uint32_t variant = 0;
        while (variant < variant_count && !same_variant(variants + variant, &candidate, zones)) ++variant;
        if (variant == variant_count) goto failed;
        const afx_c_zone_t *base = templates + variants[variant].setup;
        events[event].setup = variants[variant].setup;
        for (uint32_t field = AFX_FIELD_ENV_AD; field < AFX_FIELD_COUNT; ++field) {
            if (field == AFX_FIELD_PITCH || field == AFX_FIELD_MIX) continue;
            events[event].mask &= ~(1u << field);
            if (candidate.words[field] != zone_word(base, field)) {
                events[event].mask |= 1u << field;
                events[event].fields[field] = candidate.words[field];
            }
        }
    }
    free(variants); *out_events = events; *out_zones = templates; *out_zone_count = template_count;
    return 0;
failed:
    free(events); free(templates); free(variants); return -1;
}

static int valid_sample(const afx_c_sample_t *sample) {
    uint32_t minimum;
    if (!sample || !sample->data || !sample->frames || sample->frames > 65535 ||
        sample->format > AFX_ADPCM || sample->loop_start > sample->loop_end ||
        sample->loop_end >= sample->frames) return 0;
    minimum = sample->format == AFX_PCM16 ? sample->frames * 2u :
              sample->format == AFX_PCM8 ? sample->frames : (sample->frames + 1u) / 2u;
    return sample->bytes == minimum;
}

void afx_c_output_free(afx_c_output_t *out) {
    if (!out) return;
    free(out->afb); free(out->afx); free(out->afc); free(out->afv); *out = (afx_c_output_t){0};
}

static int seek_grow(uint8_t **data, uint32_t *capacity, uint32_t used, uint32_t add) {
    if (add > UINT32_MAX - used) return -1;
    if (used + add <= *capacity) return 0;
    uint32_t next = *capacity ? *capacity : 256;
    while (next < used + add) {
        if (next > UINT32_MAX / 2u) { next = used + add; break; }
        next *= 2u;
    }
    uint8_t *grown = realloc(*data, next);
    if (!grown) return -1;
    *data = grown; *capacity = next;
    return 0;
}

static int seek_append(uint8_t **data, uint32_t *used, uint32_t *capacity,
                       uint32_t tick, uint32_t position, uint32_t remaining,
                       const afx_restore_channel_t active[AFX_MAX_FLOW_CHANNELS],
                       const uint8_t present[AFX_MAX_FLOW_CHANNELS], uint32_t channels) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < channels; ++i) count += present[i] != 0;
    uint32_t bytes = 16u + count * (uint32_t)sizeof(afx_restore_channel_t);
    if (seek_grow(data, capacity, *used, bytes)) return -1;
    uint8_t *at = *data + *used;
    afx_write32(at, tick); afx_write32(at + 4, position);
    afx_write32(at + 8, remaining); afx_write32(at + 12, count);
    at += 16;
    for (uint32_t i = 0; i < channels; ++i) if (present[i]) {
        afx_write32(at, i);
        for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field)
            afx_write16(at + 4 + 2 * field, active[i].fields[field]);
        at += sizeof(afx_restore_channel_t);
    }
    *used += bytes;
    return 0;
}

/* The driver rebuilds from a point immediately after the next WAIT.  Build
 * that same representation from the final AFX stream so note compilers and
 * trace importers cannot drift into different AFC semantics. */
static int build_seek(afx_c_output_t *out) {
    afx_file_header_t header;
    uint64_t duration64;
    uint32_t rate_num, rate_den, at, end, tick = 0, target = 0, used = 16, capacity = 0;
    uint8_t *payload = NULL;
    uint8_t present[AFX_MAX_FLOW_CHANNELS] = {0};
    afx_restore_channel_t active[AFX_MAX_FLOW_CHANNELS] = {{0}};
    if (!out || afx_file_validate(out->afx, out->afx_bytes, &header) ||
        afx_flow_duration(out->afx, out->afx_bytes, &duration64, &rate_num, &rate_den) ||
        !rate_num || !rate_den || duration64 > UINT32_MAX) return -1;
    if (seek_grow(&payload, &capacity, 0, used)) return -1;
    afx_write32(payload, AFX_CHECKPOINT_MAGIC); afx_write32(payload + 4, AFX_CHECKPOINT_VERSION);
    at = header.stream_offset; end = at + header.stream_size;
    for (;;) {
        uint32_t position = 0, remaining = 0;
        for (;;) {
            afx_event_t event;
            if (at >= end || afx_decode_event(out->afx + header.image_offset + at, end - at, &event)) goto failed;
            if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) {
                if (event.wait > UINT32_MAX - tick) goto failed;
                if (event.wait > target - tick) {
                    position = at + event.bytes; remaining = tick + event.wait - target;
                    break;
                }
                tick += event.wait; at += event.bytes; continue;
            }
            at += event.bytes;
            if (event.opcode == AFX_OP_NOTE) {
                if (event.setup >= header.setup_count ||
                    afx_apply_setup_fields(active[event.channel].fields,
                        out->afx + header.image_offset + event.setup * AFX_SETUP_BYTES,
                        event.mask, event.values, afx_field_value_bytes(event.mask))) goto failed;
                active[event.channel].local_channel = event.channel; present[event.channel] = 1;
            } else if (event.opcode == AFX_OP_PATCH) {
                if (present[event.channel] && afx_apply_fields(active[event.channel].fields, NULL,
                    event.mask, event.values, afx_field_value_bytes(event.mask))) goto failed;
            } else if (event.opcode == AFX_OP_KEYOFF) present[event.channel] = 0;
            else if (event.opcode == AFX_OP_END || event.opcode == AFX_OP_PARK) goto failed;
            else goto failed;
        }
        if (seek_append(&payload, &used, &capacity, target, position, remaining,
                        active, present, header.required_channels)) goto failed;
        if (target >= duration64 || duration64 - target <= 1000u) break;
        target += 1000u;
    }
    afx_write32(payload + 8, 0); /* Count is written after the final entry. */
    uint32_t cursor = 16, count = 0;
    while (cursor < used) {
        uint32_t states = afx_read32(payload + cursor + 12);
        cursor += 16u + states * (uint32_t)sizeof(afx_restore_channel_t); ++count;
    }
    afx_write32(payload + 8, count);
    if (used > UINT32_MAX - AFC_HEADER) goto failed;
    out->afc_bytes = AFC_HEADER + used;
    out->afc = calloc(1, out->afc_bytes);
    if (!out->afc) goto failed;
    afx_write32(out->afc, AFX_SEEK_MAGIC); afx_write32(out->afc + 4, AFX_SEEK_VERSION);
    afx_write32(out->afc + 8, header.control_id); afx_write32(out->afc + 12, header.bank_id_low);
    afx_write32(out->afc + 16, header.bank_id_high); afx_write32(out->afc + 20, AFC_HEADER);
    afx_write32(out->afc + 24, used); afx_write32(out->afc + 28, out->afc_bytes);
    memcpy(out->afc + AFC_HEADER, payload, used); free(payload);
    return 0;
failed:
    free(payload); return -1;
}

static uint32_t visual_band(double frequency, double low, double high) {
    if (high <= low) return VISUAL_BANDS / 2u;
    int band = (int)lround((log2(frequency) - log2(low)) * (VISUAL_BANDS - 1u) /
                           (log2(high) - log2(low)));
    if (band < 0) return 0;
    return band >= VISUAL_BANDS ? VISUAL_BANDS - 1u : (uint32_t)band;
}

static double visual_frequency(uint16_t pitch_word) {
    int octave = pitch_word >> 11;
    if (octave & 8) octave -= 16;
    return 261.6256 * (1.0 + (pitch_word & 1023u) / 1024.0) * pow(2.0, octave);
}

static int visual_bounds(const uint8_t *image, const afx_file_header_t *header,
                         double *low, double *high) {
    uint32_t at = header->stream_offset, end = at + header->stream_size;
    int seen = 0;
    for (; at < end;) {
        afx_event_t event;
        uint16_t state[AFX_FIELD_COUNT];
        if (afx_decode_event(image + at, end - at, &event)) return -1;
        at += event.bytes;
        if (event.opcode != AFX_OP_NOTE) continue;
        if (event.setup >= header->setup_count ||
            afx_apply_setup_fields(state, image + event.setup * AFX_SETUP_BYTES,
                                   event.mask, event.values, afx_field_value_bytes(event.mask))) return -1;
        double frequency = visual_frequency(state[AFX_FIELD_PITCH]);
        if (!seen) { *low = *high = frequency; seen = 1; }
        else { if (frequency < *low) *low = frequency; if (frequency > *high) *high = frequency; }
    }
    return seen ? 0 : -1;
}

static int visual_pass(const uint8_t *image, const afx_file_header_t *header,
                       uint32_t frames, double low, double high,
                       double *ceiling, uint8_t *out) {
    uint16_t state[AFX_MAX_FLOW_CHANNELS][AFX_FIELD_COUNT] = {{0}};
    uint32_t started[AFX_MAX_FLOW_CHANNELS] = {0}, at = header->stream_offset;
    uint32_t end = at + header->stream_size, tick = 0;
    uint8_t active[AFX_MAX_FLOW_CHANNELS] = {0};
    for (uint32_t frame = 0; frame < frames; ++frame) {
        uint64_t target64 = (uint64_t)frame * header->tick_rate_num /
                          ((uint64_t)VISUAL_RATE * header->tick_rate_den);
        if (target64 > UINT32_MAX) return -1;
        uint32_t target = (uint32_t)target64;
        while (at < end) {
            afx_event_t event;
            if (afx_decode_event(image + at, end - at, &event)) return -1;
            if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) {
                if (event.wait > UINT32_MAX - tick || tick + event.wait > target) break;
                tick += event.wait; at += event.bytes; continue;
            }
            at += event.bytes;
            if (event.opcode == AFX_OP_NOTE) {
                if (event.setup >= header->setup_count ||
                    afx_apply_setup_fields(state[event.channel], image + event.setup * AFX_SETUP_BYTES,
                                           event.mask, event.values, afx_field_value_bytes(event.mask))) return -1;
                active[event.channel] = 1; started[event.channel] = tick;
            } else if (event.opcode == AFX_OP_PATCH) {
                if (active[event.channel] && afx_apply_fields(state[event.channel], NULL, event.mask,
                                                               event.values, afx_field_value_bytes(event.mask))) return -1;
            } else if (event.opcode == AFX_OP_KEYOFF) active[event.channel] = 0;
        }
        double levels[VISUAL_BANDS] = {0};
        for (uint32_t channel = 0; channel < AFX_MAX_FLOW_CHANNELS; ++channel) if (active[channel]) {
            uint32_t attenuation = state[channel][AFX_FIELD_TOTAL_LEVEL] >> 8;
            double age = ((uint64_t)(target - started[channel]) * header->tick_rate_den) /
                         (double)header->tick_rate_num;
            double level = pow(10.0, -(double)attenuation / 25.0) * pow(2.0, -2.0 * age / 0.6);
            levels[visual_band(visual_frequency(state[channel][AFX_FIELD_PITCH]), low, high)] += level;
        }
        for (uint32_t band = 0; band < VISUAL_BANDS; ++band) {
            if (levels[band] > *ceiling) *ceiling = levels[band];
            if (out) out[frame * VISUAL_BANDS + band] =
                (uint8_t)lround(255.0 * sqrt(levels[band] / *ceiling));
        }
    }
    return 0;
}

int afx_c_visualize(const uint8_t *afx, uint32_t bytes, uint8_t **out_visual, uint32_t *out_bytes) {
    afx_file_header_t header;
    uint64_t duration, frames64;
    uint32_t rate_num, rate_den;
    uint8_t *visual = NULL;
    double low, high, ceiling = 0;
    if (!afx || !out_visual || !out_bytes || afx_file_validate(afx, bytes, &header) ||
        !header.tick_rate_num || !header.tick_rate_den ||
        afx_flow_duration(afx, bytes, &duration, &rate_num, &rate_den) ||
        rate_num != header.tick_rate_num || rate_den != header.tick_rate_den) return -1;
    frames64 = (duration * VISUAL_RATE * header.tick_rate_den + header.tick_rate_num - 1u) /
               header.tick_rate_num;
    if (!frames64 || frames64 > (UINT32_MAX - VISUAL_HEADER) / VISUAL_BANDS ||
        visual_bounds(afx + header.image_offset, &header, &low, &high)) return -1;
    uint32_t frames = (uint32_t)frames64, visual_bytes = VISUAL_HEADER + frames * VISUAL_BANDS;
    visual = calloc(1, visual_bytes);
    if (!visual || visual_pass(afx + header.image_offset, &header, frames, low, high, &ceiling, NULL) || !ceiling ||
        visual_pass(afx + header.image_offset, &header, frames, low, high, &ceiling, visual + VISUAL_HEADER)) {
        free(visual); return -1;
    }
    memcpy(visual, "VIZ1", 4); visual[4] = 1; visual[5] = VISUAL_BANDS;
    visual[6] = VISUAL_RATE; afx_write32(visual + 8, frames);
    *out_visual = visual; *out_bytes = visual_bytes;
    return 0;
}

static int assemble_output(const afx_c_zone_t *zones, uint32_t zone_count,
                           const afx_c_zone_t *bank_zones, uint32_t bank_zone_count,
                           const uint8_t *stream, uint32_t stream_bytes,
                           uint32_t channels, uint32_t tick_rate, int controlled, afx_c_output_t *out) {
    uint32_t *offsets = NULL, sample_bytes = 0;
    if (!zones || !zone_count || zone_count > UINT16_MAX ||
        !bank_zones || !bank_zone_count || bank_zone_count > UINT16_MAX || !stream || !stream_bytes ||
        !channels || channels > AFX_MAX_FLOW_CHANNELS || !tick_rate || !out) return -1;
    /* AFB/AFI layout follows the source zones, independently of the optimized
       setup dictionary. Songs sharing these zones must get identical banks. */
    offsets = malloc(bank_zone_count * sizeof(*offsets));
    if (!offsets) goto failed;
    for (uint32_t i = 0; i < bank_zone_count; ++i) {
        if (bank_zones[i].key_min > bank_zones[i].key_max ||
            bank_zones[i].velocity_min > bank_zones[i].velocity_max ||
            !valid_sample(&bank_zones[i].sample)) goto failed;
        for (uint32_t prior = 0; prior < i; ++prior)
            if (bank_zones[i].sample.data == bank_zones[prior].sample.data &&
                bank_zones[i].sample.bytes == bank_zones[prior].sample.bytes) {
                offsets[i] = offsets[prior]; goto sample_known;
            }
        sample_bytes = align32(sample_bytes); offsets[i] = sample_bytes;
        if (bank_zones[i].sample.bytes > UINT32_MAX - sample_bytes) goto failed;
        sample_bytes += bank_zones[i].sample.bytes;
sample_known:;
    }
    if (sample_bytes > UINT32_MAX - AFB_HEADER) goto failed;
    out->afb_bytes = AFB_HEADER + sample_bytes; out->afb = calloc(1, out->afb_bytes);
    if (!out->afb) goto failed;
    for (uint32_t i = 0; i < bank_zone_count; ++i) {
        int first = 1;
        for (uint32_t prior = 0; prior < i; ++prior)
            if (offsets[prior] == offsets[i]) { first = 0; break; }
        if (first) memcpy(out->afb + AFB_HEADER + offsets[i], bank_zones[i].sample.data, bank_zones[i].sample.bytes);
    }
    uint32_t bank_id = hash32(out->afb + AFB_HEADER, sample_bytes);
    afx_write32(out->afb, AFX_BANK_MAGIC); afx_write32(out->afb + 4, AFX_BANK_VERSION);
    afx_write32(out->afb + 8, bank_id); afx_write32(out->afb + 12, hash32_alt(out->afb + AFB_HEADER, sample_bytes));
    afx_write32(out->afb + 16, AFB_HEADER); afx_write32(out->afb + 20, sample_bytes);
    afx_write32(out->afb + 24, out->afb_bytes);
    uint32_t image_at = align32(AFX_HEADER + zone_count * RELOCATION_BYTES);
    if (stream_bytes > UINT32_MAX - zone_count * AFX_SETUP_BYTES) goto failed;
    uint32_t image_bytes = zone_count * AFX_SETUP_BYTES + stream_bytes;
    if (image_bytes > UINT32_MAX - image_at) goto failed;
    out->afx_bytes = image_at + image_bytes; out->afx = calloc(1, out->afx_bytes);
    if (!out->afx) goto failed;
    uint8_t *afx = out->afx, *setup = afx + image_at;
    for (uint32_t i = 0; i < zone_count; ++i) {
        uint32_t sample = 0;
        while (sample < bank_zone_count &&
               (zones[i].sample.data != bank_zones[sample].sample.data ||
                zones[i].sample.bytes != bank_zones[sample].sample.bytes)) ++sample;
        if (sample == bank_zone_count) goto failed;
        uint8_t *state = setup + i * AFX_SETUP_BYTES;
        uint32_t address = offsets[sample];
        if (address > 0x7fffffu) goto failed;
        afx_write16(state, (zones[i].sample.loop ? 0x0200 : 0) |
                    ((uint16_t)zones[i].sample.format << 7) | (address >> 16));
        afx_write16(state + 2, address);
        afx_write16(state + 4, zones[i].sample.loop_start); afx_write16(state + 6, zones[i].sample.loop_end);
        afx_write16(state + 8, 0x001f); afx_write16(state + 10, 0x001f);
        afx_write16(state + 16, zones[i].dsp_send); afx_write16(state + 18, 0x0f10);
        afx_write16(state + 20, 0x0024);
        for (uint32_t field = 11; field < 16; ++field) afx_write16(state + 2 * field, 0x1fffu);
        for (uint32_t field = 4; field < AFX_FIELD_COUNT; ++field)
            if (zones[i].setup_mask & (1u << field)) afx_write16(state + 2 * field, zones[i].setup[field]);
        uint8_t *relocation = afx + AFX_HEADER + i * RELOCATION_BYTES;
        afx_write32(relocation, i * AFX_SETUP_BYTES); afx_write32(relocation + 4, address);
        afx_write32(relocation + 8, zones[i].sample.bytes);
    }
    memcpy(setup + zone_count * AFX_SETUP_BYTES, stream, stream_bytes);
    uint32_t control_id = afx_control_id(setup, image_bytes);
    afx_write32(afx, AFX_FILE_MAGIC); afx_write32(afx + 4, AFX_FILE_VERSION); afx_write32(afx + 8, out->afx_bytes);
    afx_write32(afx + 12, controlled ? AFX_FLAG_CONTROLLED : AFX_FLAG_MUSIC); afx_write32(afx + 16, image_at); afx_write32(afx + 20, image_bytes);
    afx_write32(afx + 24, zone_count * AFX_SETUP_BYTES); afx_write32(afx + 28, stream_bytes); afx_write32(afx + 32, control_id);
    afx_write32(afx + 36, zone_count); afx_write32(afx + 40, bank_id); afx_write32(afx + 44, afx_read32(out->afb + 12));
    afx_write32(afx + 48, AFX_HEADER); afx_write32(afx + 52, zone_count); afx_write32(afx + 64, channels);
    afx_write32(afx + 68, tick_rate); afx_write32(afx + 72, 1);
    if (!controlled && (build_seek(out) || afx_c_visualize(out->afx, out->afx_bytes, &out->afv, &out->afv_bytes))) goto failed;
    free(offsets); return 0;
failed:
    free(offsets); afx_c_output_free(out); return -1;
}

static int compile_events(const afx_c_event_t *input, uint32_t count,
                          uint32_t duration_ticks, uint32_t tick_rate,
                          const afx_c_zone_t *zones, uint32_t zone_count,
                          const afx_c_zone_t *bank_zones, uint32_t bank_zone_count,
                          afx_c_output_t *out);

int afx_c_compile_zones(const afx_c_note_t *input, uint32_t count,
                        uint32_t tick_rate, const afx_c_zone_t *zones,
                        uint32_t zone_count, afx_c_output_t *out) {
    if (!out || !input || !count || !tick_rate || !zones || !zone_count || zone_count > UINT16_MAX)
        return -1;
    *out = (afx_c_output_t){0};
    afx_c_note_t *notes = malloc(count * sizeof(*notes));
    event_t *events = malloc(2u * count * sizeof(*events));
    afx_c_event_t *raw = NULL, *optimized = NULL;
    afx_c_zone_t *templates = NULL;
    uint8_t *channels = NULL;
    uint32_t template_count = 0, channel_count = 0;
    if (!notes || !events || count > UINT32_MAX / 2u) goto failed;
    raw = calloc(2u * count, sizeof(*raw));
    if (!raw) goto failed;
    memcpy(notes, input, count * sizeof(*notes)); qsort(notes, count, sizeof(*notes), compare_note);
    if (afx_c_schedule_notes(notes, count, AFX_EXECUTION_BUDGET_COMMANDS)) goto failed;
    channels = malloc(count);
    if (!channels || afx_c_assign_channels(notes, count, channels, &channel_count)) goto failed;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t keyoff = notes[i].release_tick ? notes[i].release_tick : notes[i].end_tick;
        if (!notes[i].velocity || keyoff <= notes[i].start_tick || notes[i].end_tick < keyoff) goto failed;
        uint32_t setup = zone_count;
        if (notes[i].setup_index) {
            if (notes[i].setup_index > zone_count) goto failed;
            setup = notes[i].setup_index - 1u;
        } else for (uint32_t scan = 0; scan < zone_count; ++scan)
            if (zones[scan].bank_msb == notes[i].bank_msb &&
                zones[scan].bank_lsb == notes[i].bank_lsb &&
                zones[scan].program == notes[i].program &&
                zones[scan].key_min <= notes[i].key && notes[i].key <= zones[scan].key_max) {
                if (notes[i].velocity < zones[scan].velocity_min ||
                    notes[i].velocity > zones[scan].velocity_max) continue;
                if (setup != zone_count) goto failed;
                setup = scan;
            }
        if (setup == zone_count) goto failed;
        events[2 * i] = (event_t){notes[i].start_tick, 1, channels[i], notes[i].key, notes[i].velocity,
                                  (uint16_t)setup, notes[i].mix, notes[i].controller_state >> 39 ?
                                  (int16_t)(((notes[i].controller_state >> 25) & 16383u) - 8192) : 0};
        events[2 * i + 1] = (event_t){keyoff, 0, channels[i], 0, 0, 0, 0, 0};
    }
    qsort(events, 2u * count, sizeof(*events), compare_event);
    uint32_t final_end = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (notes[i].end_tick > final_end) final_end = notes[i].end_tick;
    for (uint32_t i = 0; i < 2u * count; ++i) {
        raw[i] = (afx_c_event_t){.tick = events[i].tick, .order = i,
                                  .channel = events[i].channel,
                                  .opcode = events[i].kind ? AFX_OP_NOTE : AFX_OP_KEYOFF};
        if (events[i].kind) {
            raw[i].setup = events[i].setup; raw[i].mask = AFX_NOTE_PL_MASK;
            int tuning = zones[events[i].setup].sample.tuning_cents;
            /* MIDI's default bend range is ±2 semitones. Both SF2 pitch
             * correction and positive bend raise the playback ratio. */
            tuning += (int)lround((double)events[i].pitch_bend * 200.0 / 8192.0);
            raw[i].fields[AFX_FIELD_PITCH] = pitch(events[i].key, zones[events[i].setup].sample.root_key,
                                                    tuning, zones[events[i].setup].sample.sample_rate);
            raw[i].fields[AFX_FIELD_MIX] = events[i].mix ? events[i].mix : level(events[i].velocity);
        }
    }
    if (afx_c_optimize_events(raw, 2u * count, zones, zone_count, &optimized, &templates, &template_count) ||
        compile_events(optimized, 2u * count, final_end, tick_rate, templates, template_count,
                       zones, zone_count, out)) goto failed;
    free(channels); free(notes); free(events); free(raw); free(optimized); free(templates); return 0;
failed:
    free(channels); free(notes); free(events); free(raw); free(optimized); free(templates);
    afx_c_output_free(out); return -1;
}

static int compare_control_event(const void *left, const void *right) {
    const afx_c_event_t *a = left, *b = right;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

static int compile_events(const afx_c_event_t *input, uint32_t count,
                          uint32_t duration_ticks, uint32_t tick_rate,
                          const afx_c_zone_t *zones, uint32_t zone_count,
                          const afx_c_zone_t *bank_zones, uint32_t bank_zone_count,
                          afx_c_output_t *out) {
    afx_c_event_t *events = NULL;
    uint8_t *stream = NULL;
    uint32_t cursor = 0, previous = 0, channels = 0, commands = 0, writes = 0;
    if (!out || !input || !count || !tick_rate || !zones || !zone_count ||
        count > (UINT32_MAX - 6u) / 49u) return -1;
    *out = (afx_c_output_t){0};
    events = malloc(count * sizeof(*events)); stream = malloc(count * 49u + 6u);
    if (!events || !stream) goto failed;
    memcpy(events, input, count * sizeof(*events)); qsort(events, count, sizeof(*events), compare_control_event);
    for (uint32_t i = 0; i < count; ++i) {
        afx_c_event_t *source = &events[i];
        if (source->tick > duration_ticks || (i && source->tick == events[i - 1].tick &&
                                               source->order == events[i - 1].order) ||
            source->channel >= AFX_MAX_FLOW_CHANNELS || (source->mask & ~AFX_FIELD_MASK)) goto failed;
        if (source->opcode == AFX_OP_NOTE) {
            if (source->setup >= zone_count) goto failed;
            writes += NOTE_WRITES;
        } else if (source->opcode == AFX_OP_PATCH) {
            if (!source->mask) goto failed;
            writes += afx_field_value_bytes(source->mask) / 2u;
        } else if (source->opcode == AFX_OP_KEYOFF) {
            if (source->mask) goto failed;
            ++writes;
        } else if (source->opcode == AFX_OP_PARK) {
            if (source->mask || !i || i + 1u != count || source->tick != duration_ticks) goto failed;
        } else goto failed;
        ++commands;
        if (commands > AFX_EXECUTION_BUDGET_COMMANDS || writes > AFX_EXECUTION_BUDGET_WRITES) goto failed;
        if (i + 1u == count || events[i + 1].tick != source->tick) commands = writes = 0;
        cursor += encode_wait(stream + cursor, source->tick - previous); previous = source->tick;
        afx_event_t wire = {.opcode = source->opcode, .channel = source->channel,
                            .setup = source->setup, .mask = source->mask};
        if (wire.opcode == AFX_OP_NOTE && wire.mask == AFX_NOTE_PL_MASK) wire.opcode = AFX_OP_NOTE_PL;
        if (wire.opcode == AFX_OP_PATCH && wire.mask == (1u << AFX_FIELD_TOTAL_LEVEL)) wire.opcode = AFX_OP_PATCH_LEVEL;
        uint16_t values[AFX_FIELD_COUNT]; uint32_t selected = 0, written = 0;
        for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field)
            if (source->mask & (1u << field)) values[selected++] = source->fields[field];
        if (afx_encode_event(stream + cursor, count * 49u + 6u - cursor, &wire, values, &written)) goto failed;
        cursor += written;
        if (source->channel + 1u > channels) channels = source->channel + 1u;
    }
    int controlled = events[count - 1u].opcode == AFX_OP_PARK;
    if (!controlled) { cursor += encode_wait(stream + cursor, duration_ticks - previous); stream[cursor++] = AFX_OP_END; }
    if (assemble_output(zones, zone_count, bank_zones, bank_zone_count,
                        stream, cursor, channels, tick_rate, controlled, out)) goto failed;
    free(events); free(stream); return 0;
failed:
    free(events); free(stream); afx_c_output_free(out); return -1;
}

int afx_c_compile_events(const afx_c_event_t *input, uint32_t count,
                         uint32_t duration_ticks, uint32_t tick_rate,
                         const afx_c_zone_t *zones, uint32_t zone_count,
                         afx_c_output_t *out) {
    return compile_events(input, count, duration_ticks, tick_rate, zones, zone_count,
                          zones, zone_count, out);
}

int afx_c_compile_sample(const afx_c_note_t *notes, uint32_t count,
                         uint32_t tick_rate, const afx_c_sample_t *sample,
                         afx_c_output_t *out) {
    if (!sample) return -1;
    const afx_c_zone_t zone = {.sample = *sample, .key_max = 127, .velocity_max = 127};
    return afx_c_compile_zones(notes, count, tick_rate, &zone, 1, out);
}

int afx_c_compile_sine(const afx_c_note_t *notes, uint32_t count,
                       uint32_t tick_rate, afx_c_output_t *out) {
    uint8_t pcm[SINE_FRAMES * 2];
    for (uint32_t i = 0; i < SINE_FRAMES; ++i) {
        int16_t value = (int16_t)(sin(6.28318530717958647692 * i / SINE_FRAMES) * 28000.0);
        afx_write16(pcm + 2 * i, (uint16_t)value);
    }
    const afx_c_sample_t sample = {pcm, sizeof(pcm), SINE_FRAMES, AFX_PCM16, 69, 1,
                                   0, SINE_FRAMES - 1, 0, 44100};
    return afx_c_compile_sample(notes, count, tick_rate, &sample, out);
}
