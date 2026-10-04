#include "afx_compile_c.h"
#include "afx_midi_c.h"
#include "afx_sample_c.h"
#include <aicaflow/codec.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void assert_stream_budget(const afx_c_output_t *out) {
    afx_file_header_t header;
    assert(afx_file_validate(out->afx, out->afx_bytes, &header) == AFX_OK);
    uint32_t at = header.image_offset + header.stream_offset;
    uint32_t end = at + header.stream_size, commands = 0, writes = 0;
    while (at < end) {
        afx_event_t event;
        assert(afx_decode_event(out->afx + at, end - at, &event) == AFX_OK);
        at += event.bytes;
        if (event.opcode >= AFX_OP_WAIT8 && event.opcode <= AFX_OP_WAIT32) {
            assert(commands <= AFX_EXECUTION_BUDGET_COMMANDS);
            assert(writes <= AFX_EXECUTION_BUDGET_WRITES);
            commands = writes = 0;
        } else if (event.opcode == AFX_OP_NOTE) {
            ++commands; writes += 19;
        } else if (event.opcode == AFX_OP_KEYOFF) {
            ++commands; ++writes;
        }
    }
    assert(commands <= AFX_EXECUTION_BUDGET_COMMANDS);
    assert(writes <= AFX_EXECUTION_BUDGET_WRITES);
}

int main(void) {
    const afx_c_note_t notes[] = {{.start_tick = 0, .end_tick = 500, .key = 69, .velocity = 120},
                                  {.start_tick = 250, .end_tick = 750, .key = 76, .velocity = 100}};
    afx_c_output_t out;
    assert(!afx_c_compile_sine(notes, 2, 1000, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(afx_read16(out.afx + afx_read32(out.afx + 16) + 18) == 0x0f10);
    assert(out.afc_bytes > 64 && afx_read32(out.afc) == AFX_SEEK_MAGIC);
    assert(afx_read32(out.afc + 32) == AFX_CHECKPOINT_MAGIC &&
           afx_read32(out.afc + 40) == 1 && afx_read32(out.afc + 60) == 1);
    assert(out.afv_bytes > 12 && !memcmp(out.afv, "VIZ1", 4));
    uint8_t *visual = NULL;
    uint32_t visual_bytes = 0;
    assert(!afx_c_visualize(out.afx, out.afx_bytes, &visual, &visual_bytes));
    assert(visual_bytes == out.afv_bytes && !memcmp(visual, out.afv, visual_bytes));
    free(visual);
    /* Both active pitches cover the visual range; a later, quieter note must
       still light a different band instead of collapsing a chord to one bar. */
    assert(out.afv[12] > 0 && out.afv[12 + 15 * 32] > 0 && out.afv[12 + 15 * 32 + 31] > 0);
    afx_c_output_t repeat;
    assert(!afx_c_compile_sine(notes, 2, 1000, &repeat));
    assert(out.afb_bytes == repeat.afb_bytes && !memcmp(out.afb, repeat.afb, out.afb_bytes));
    assert(out.afx_bytes == repeat.afx_bytes && !memcmp(out.afx, repeat.afx, out.afx_bytes));
    assert(out.afc_bytes == repeat.afc_bytes && !memcmp(out.afc, repeat.afc, out.afc_bytes));
    assert(out.afv_bytes == repeat.afv_bytes && !memcmp(out.afv, repeat.afv, out.afv_bytes));
    afx_c_output_free(&repeat);
    afx_c_output_free(&out);
    const uint8_t pcm[] = {0, 0, 0xff, 0x7f, 0, 0, 0, 0x80};
    const afx_c_sample_t one_shot = {pcm, sizeof(pcm), 4, AFX_PCM16, 60, 0, 0, 3, 0, 44100};
    assert(!afx_c_compile_sample(notes, 2, 1000, &one_shot, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(out.afb_bytes == 32 + sizeof(pcm) && !memcmp(out.afb + 32, pcm, sizeof(pcm)));
    afx_c_output_free(&out);
    /* The pitch word compensates for an offline sample-rate choice. Without
       this, 22.05 kHz assets play an octave too high on AICA's 44.1 kHz base. */
    const afx_c_note_t root_note[] = {{.start_tick = 0, .end_tick = 100, .key = 60, .velocity = 100}};
    const afx_c_sample_t half_rate = {pcm, sizeof(pcm), 4, AFX_PCM16, 60, 0, 0, 3, 0, 22050};
    assert(!afx_c_compile_sample(root_note, 1, 1000, &half_rate, &out));
    uint32_t half_image = afx_read32(out.afx + 16), half_stream = afx_read32(out.afx + 24);
    assert(out.afx[half_image + half_stream] == AFX_OP_NOTE_PL &&
           ((afx_read16(out.afx + half_image + half_stream + 4) >> 11) & 15u) == 15u);
    afx_c_output_free(&out);
    uint8_t *encoded = NULL, format = 0;
    uint32_t encoded_bytes = 0;
    assert(!afx_c_encode_sample(pcm, 4, 0, AFX_PCM8, &encoded, &encoded_bytes, &format));
    assert(format == AFX_PCM8 && encoded_bytes == 4); free(encoded);
    assert(!afx_c_encode_sample(pcm, 4, 0, AFX_ADPCM, &encoded, &encoded_bytes, &format));
    assert(format == AFX_ADPCM && encoded_bytes == 2); free(encoded);
    assert(!afx_c_encode_sample(pcm, 4, 1, AFX_SAMPLE_AUTO, &encoded, &encoded_bytes, &format));
    assert(format == AFX_PCM8); free(encoded);
    /* Matches the retired Python/SciPy resample_poly(3, 2) reference.  The
       C authoring path must not silently fall back to the old linear sampler. */
    const uint8_t resample_source[] = {
        0,0, 0xe8,3, 0x30,0xf8, 0xb8,0xb, 0x60,0xf0, 0x88,0x13, 0x90,0xe8, 0x58,0x1b
    };
    const uint8_t resample_expected[] = {
        0,0, 0xdd,8, 0x4e,0xfa, 0x2f,0xf8, 0xa4,0xd, 0xa0,0xfe,
        0x5e,0xf0, 0x83,0xe, 0x43,0x7, 0x8c,0xe8, 0x8c,0x7, 0x6,0x1f
    };
    uint8_t *resampled = NULL;
    uint32_t resampled_frames = 0;
    assert(!afx_c_resample_pcm16(resample_source, 8, 2, 3, &resampled, &resampled_frames));
    assert(resampled_frames == 12 && !memcmp(resampled, resample_expected, sizeof(resample_expected)));
    free(resampled);
    /* This is the actual 44.1 kHz -> 27 kHz ratio used by the cello bank.
       An impulse exercises the long polyphase filter rather than just its
       short edge behaviour. */
    const uint8_t cello_rate_source[74] = {
        [24] = 0xe0, [25] = 0x2e, [26] = 0x60, [27] = 0xf0,
        [28] = 0xd0, [29] = 0x07
    };
    const uint8_t cello_rate_expected[] = {
        0xc2,0xff,0x69,0,0x57,0xff,0x08,1,0x65,0xfe,0xa2,2,0xe7,0xfa,0x01,0x16,
        0x54,5,0x35,0xff,0x70,1,0x02,0xff,0xa6,0,0x98,0xff,0x3e,0,0xdf,0xff,
        0x0f,0,0xfc,0xff,0xfd,0xff,0,0,0,0,0,0,0,0
    };
    assert(!afx_c_resample_pcm16(cello_rate_source, 37, 44100, 27000,
                                 &resampled, &resampled_frames));
    assert(resampled_frames == 23 && !memcmp(resampled, cello_rate_expected, sizeof(cello_rate_expected)));
    free(resampled);
    const afx_c_note_t split_notes[] = {{.start_tick = 0, .end_tick = 100, .key = 60, .velocity = 100},
                                        {.start_tick = 100, .end_tick = 200, .key = 72, .velocity = 100}};
    const afx_c_zone_t zones[] = {
        {.sample = {pcm, sizeof(pcm), 4, AFX_PCM16, 60, 0, 0, 3, 0, 44100},
         .key_min = 0, .key_max = 65, .velocity_max = 127},
        {.sample = {pcm, sizeof(pcm), 4, AFX_PCM16, 72, 1, 0, 3, 0, 44100},
         .key_min = 66, .key_max = 127, .velocity_max = 127},
    };
    assert(!afx_c_compile_zones(split_notes, 2, 1000, zones, 2, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(afx_read32(out.afx + 36) == 2 && afx_read32(out.afx + 52) == 2);
    assert(afx_read32(out.afx + 80 + 4) == 0 && afx_read32(out.afx + 92 + 4) == 0);
    assert(out.afb_bytes == 32 + sizeof(pcm));
    afx_c_output_free(&out);
    /* Setup optimization must not reorder or drop bank samples: AFI indices
       and other songs sharing this bank use the original zone order. */
    const uint8_t bank_pcm[][8] = {{1}, {2}, {3}};
    afx_c_zone_t bank_zones[4];
    for (unsigned i = 0; i < 4; ++i) {
        bank_zones[i] = zones[0];
        bank_zones[i].sample.data = bank_pcm[i % 3];
    }
    const afx_c_note_t bank_notes[] = {
        {.start_tick = 0, .end_tick = 100, .key = 60, .velocity = 100, .setup_index = 3},
        {.start_tick = 100, .end_tick = 200, .key = 60, .velocity = 100, .setup_index = 4},
    };
    assert(!afx_c_compile_zones(bank_notes, 2, 1000, bank_zones, 4, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    assert(out.afb_bytes == 32 + 64 + sizeof(bank_pcm[0]));
    for (unsigned i = 0; i < 3; ++i)
        assert(!memcmp(out.afb + 32 + i * 32, bank_pcm[i], sizeof(bank_pcm[i])));
    assert(afx_read32(out.afx + 36) == 2);
    assert(afx_read32(out.afx + 80 + 4) == 64 && afx_read32(out.afx + 92 + 4) == 0);
    assert(!afx_c_compile_zones(bank_notes + 1, 1, 1000, bank_zones, 4, &repeat));
    assert(out.afb_bytes == repeat.afb_bytes && !memcmp(out.afb, repeat.afb, out.afb_bytes));
    afx_c_output_free(&repeat);
    afx_c_output_free(&out);
    /* A dense MIDI chord is emitted in adjacent millisecond slices rather
       than producing an AICAflow which ARM7 cannot activate. */
    afx_c_note_t dense[10];
    for (unsigned i = 0; i < 10; ++i)
        dense[i] = (afx_c_note_t){.start_tick = 0, .end_tick = 100, .key = (uint8_t)(60 + i), .velocity = 100};
    assert(!afx_c_compile_sample(dense, 10, 1000, &one_shot, &out));
    assert_stream_budget(&out);
    afx_c_output_free(&out);
    /* Trace importers use the same bank writer, but retain live register
       updates instead of reducing their source to MIDI notes. */
    const afx_c_zone_t trace_zone[] = {{
        .sample = one_shot, .key_max = 127, .velocity_max = 127
    }};
    const afx_c_event_t trace_events[] = {
        {.tick = 0, .order = 0, .opcode = AFX_OP_NOTE, .channel = 0, .setup = 0,
         .mask = AFX_NOTE_PL_MASK, .fields = {[AFX_FIELD_PITCH] = 0x0800, [AFX_FIELD_MIX] = 0x3024}},
        {.tick = 10, .order = 0, .opcode = AFX_OP_PATCH, .channel = 0,
         .mask = 1u << AFX_FIELD_MIX, .fields = {[AFX_FIELD_MIX] = 0x5024}},
        {.tick = 20, .order = 0, .opcode = AFX_OP_KEYOFF, .channel = 0},
    };
    assert(!afx_c_compile_events(trace_events, 3, 30, 1000, trace_zone, 1, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    uint32_t trace_image = afx_read32(out.afx + 16);
    assert(out.afx[trace_image + AFX_SETUP_BYTES] == AFX_OP_NOTE_PL &&
           out.afx[trace_image + AFX_SETUP_BYTES + 10] == AFX_OP_PATCH_LEVEL);
    afx_c_output_free(&out);
    /* A release tail occupies its channel, but KEYOFF remains at the musical
       note end.  This is how the offline bank builder preserves an SF2
       release without inventing an ARM7-specific command. */
    const afx_c_note_t tailed[] = {
        {.start_tick = 0, .end_tick = 1100, .release_tick = 100, .key = 60, .velocity = 100},
        {.start_tick = 500, .end_tick = 600, .release_tick = 600, .key = 72, .velocity = 100},
    };
    assert(!afx_c_compile_sample(tailed, 2, 1000, &one_shot, &out));
    assert(afx_read32(out.afx + 64) == 2);
    afx_file_header_t tailed_header;
    assert(afx_file_validate(out.afx, out.afx_bytes, &tailed_header) == AFX_OK);
    uint32_t tailed_at = tailed_header.image_offset + tailed_header.stream_offset;
    uint32_t tailed_end = tailed_at + tailed_header.stream_size, tailed_tick = 0;
    int keyoff_at_note_end = 0;
    while (tailed_at < tailed_end) {
        afx_event_t event;
        assert(afx_decode_event(out.afx + tailed_at, tailed_end - tailed_at, &event) == AFX_OK);
        tailed_at += event.bytes;
        if (event.opcode == AFX_OP_WAIT8 || event.opcode == AFX_OP_WAIT16 || event.opcode == AFX_OP_WAIT32)
            tailed_tick += event.wait;
        else if (event.opcode == AFX_OP_KEYOFF && tailed_tick == 100) keyoff_at_note_end = 1;
        if (event.opcode == AFX_OP_END) break;
    }
    assert(keyoff_at_note_end);
    /* The control stream must remain alive through the release tail; ending
       directly after the final KEYOFF truncates it in the runtime. */
    assert(tailed_tick == 1100);
    afx_c_output_free(&out);
    /* Static source controls lower into ordinary setup words, while a source
       velocity law can supply the NOTE's final MIX word. */
    const afx_c_note_t controlled_note[] = {{.start_tick = 0, .end_tick = 100, .key = 60,
                                               .velocity = 100, .mix = 0x4324}};
    const afx_c_zone_t controlled_zone[] = {{
        .sample = {pcm, sizeof(pcm), 4, AFX_PCM16, 60, 0, 0, 3, 0, 44100},
        .key_max = 127, .velocity_max = 127, .dsp_send = 0x70,
        .setup_mask = (1u << AFX_FIELD_ENV_AD) | (1u << AFX_FIELD_DIRECT),
        .setup = {[AFX_FIELD_ENV_AD] = 0x1234, [AFX_FIELD_DIRECT] = 0x0f10}
    }};
    assert(!afx_c_compile_zones(controlled_note, 1, 1000, controlled_zone, 1, &out));
    uint32_t image = afx_read32(out.afx + 16);
    assert(afx_read16(out.afx + image + 8) == 0x1234 && afx_read16(out.afx + image + 16) == 0x70);
    assert(afx_read16(out.afx + image + 36 + 6) == 0x4324);
    afx_c_output_free(&out);
    /* MIDI/SF2 lowering selects both zones, but the common compiler keeps one
       sample template and emits only the differing DIRECT word at NOTE-on. */
    const afx_c_note_t templated_notes[] = {
        {.start_tick = 0, .end_tick = 100, .key = 60, .velocity = 100, .setup_index = 1},
        {.start_tick = 100, .end_tick = 200, .key = 60, .velocity = 100, .setup_index = 2},
    };
    const afx_c_zone_t templated_zones[] = {
        {.sample = one_shot, .key_max = 127, .velocity_max = 127,
         .setup_mask = 1u << AFX_FIELD_DIRECT, .setup = {[AFX_FIELD_DIRECT] = 0x0f10}},
        {.sample = one_shot, .key_max = 127, .velocity_max = 127,
         .setup_mask = 1u << AFX_FIELD_DIRECT, .setup = {[AFX_FIELD_DIRECT] = 0x0f11}},
    };
    assert(!afx_c_compile_zones(templated_notes, 2, 1000, templated_zones, 2, &out));
    assert(afx_read32(out.afx + 36) == 1);
    afx_file_header_t templated_header;
    assert(afx_file_validate(out.afx, out.afx_bytes, &templated_header) == AFX_OK);
    uint32_t templated_at = templated_header.image_offset + templated_header.stream_offset;
    uint32_t templated_end = templated_at + templated_header.stream_size, full_note = 0;
    while (templated_at < templated_end) {
        afx_event_t event;
        assert(afx_decode_event(out.afx + templated_at, templated_end - templated_at, &event) == AFX_OK);
        templated_at += event.bytes;
        if (event.opcode == AFX_OP_NOTE && event.mask & (1u << AFX_FIELD_DIRECT)) ++full_note;
    }
    assert(full_note == 1);
    afx_c_output_free(&out);
    const unsigned char midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,20,
        0,0xff,0x51,3,9,39,192, 0,0x90,69,100,
        0x83,0x60,0x80,69,0, 0,0xff,0x2f,0
    };
    afx_c_note_t *parsed = NULL;
    unsigned count = 0;
    assert(!afx_c_midi_notes(midi, sizeof(midi), 1000, &parsed, &count));
    assert(count == 1 && parsed[0].start_tick == 0 && parsed[0].end_tick == 600);
    assert(!afx_c_compile_sine(parsed, count, 1000, &out));
    assert(afx_file_validate(out.afx, out.afx_bytes, NULL) == AFX_OK);
    free(parsed); afx_c_output_free(&out);
    /* Repeated NOTE-on for one MIDI key is a FIFO stack, not an implicit
       NOTE-off. Both notes must keep their own later key release. */
    const unsigned char repeated_key_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,20,
        0,0x90,60,100, 0,0x90,60,90, 0x60,0x80,60,0, 0x60,0x80,60,0, 0,0xff,0x2f,0
    };
    assert(!afx_c_midi_notes(repeated_key_midi, sizeof(repeated_key_midi), 1000, &parsed, &count));
    assert(count == 2 && parsed[0].end_tick == 100 && parsed[1].end_tick == 200);
    free(parsed);
    /* Volume, expression and bend are source state at NOTE-on. The C SF2
       lowerer consumes them offline; ARM7 never interprets MIDI controllers. */
    const unsigned char controller_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,28,
        0,0xb0,7,64, 0,0xb0,11,63, 0,0xb0,91,100, 0,0xe0,0,65, 0,0x90,69,100,
        0x60,0x80,69,0, 0,0xff,0x2f,0
    };
    assert(!afx_c_midi_notes(controller_midi, sizeof(controller_midi), 1000, &parsed, &count));
    assert(count == 1 && parsed[0].controller_state >> 39 &&
           ((parsed[0].controller_state >> 4) & 127u) == 64 &&
           ((parsed[0].controller_state >> 11) & 127u) == 63 &&
           ((parsed[0].controller_state >> 18) & 127u) == 64 &&
           ((parsed[0].controller_state >> 25) & 16383u) == 8320 &&
           parsed[0].controllers[91] == 100);
    free(parsed);
    /* Type-1 tracks are one MIDI channel timeline: a program event in the
       control track must apply to notes placed in another track. */
    const unsigned char split_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,1, 0,2, 1,224,
        'M','T','r','k', 0,0,0,12,
        0,0x90,60,100, 0x60,0x80,60,0, 0,0xff,0x2f,0,
        'M','T','r','k', 0,0,0,7,
        0,0xc0,42, 0,0xff,0x2f,0
    };
    assert(!afx_c_midi_notes(split_midi, sizeof(split_midi), 1000, &parsed, &count));
    assert(count == 1 && parsed[0].program == 42 && parsed[0].end_tick == 100);
    free(parsed);
    /* Sustain pedal is baked into the offline note lifetime.  The ARM7 sees
       only the resulting KEYOFF at pedal release, exactly as for a native
       long note. */
    const unsigned char sustain_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 1,224,
        'M','T','r','k', 0,0,0,20,
        0,0x90,60,100, 10,0xb0,64,127, 10,0x80,60,0,
        0x50,0xb0,64,0, 0,0xff,0x2f,0
    };
    assert(!afx_c_midi_notes(sustain_midi, sizeof(sustain_midi), 1000, &parsed, &count));
    assert(count == 1 && parsed[0].start_tick == 0 && parsed[0].end_tick == 104);
    free(parsed);
    return 0;
}
