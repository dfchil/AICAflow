/* Diagnostic inputs, generated through the real AICAforge compiler. */
#include "afx_compile_c.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void save(const char *dir, const char *name, const void *data, unsigned bytes) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(data, 1, bytes, f) == bytes);
    assert(!fclose(f));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    enum { CLICK, SINE, SAW, LFO, CARRIER, RAMP0, RAMP1, WINDOW0, WINDOW1, COUNT };
    int16_t samples[COUNT][2048] = {0};
    afx_c_zone_t zones[COUNT] = {0};
    for (unsigned i = 0; i < 2048; ++i) {
        double phase = i / 2048.0;
        samples[CLICK][i] = i < 32 ? (int16_t)(24000 * sin(6.28318530718 * i / 16) * (32-i)/32) : 0;
        samples[SINE][i] = (int16_t)(28000 * sin(6.28318530718 * phase));
        samples[SAW][i] = (int16_t)(18000 * (2 * phase - 1));
        samples[LFO][i] = (int16_t)(30000 * sin(6.28318530718 * phase));
        samples[CARRIER][i] = samples[LFO][i];
        for (unsigned c = 0; c < 2; ++c) {
            double p = fmod(phase + c * .5, 1);
            samples[RAMP0+c][i] = (int16_t)(30000 * (2*p-1));
            samples[WINDOW0+c][i] = (int16_t)(30000 * (.5 - .5*cos(6.28318530718*p)));
        }
    }
    for (unsigned i = 0; i < COUNT; ++i) {
        unsigned rate = i == CLICK ? 44100 : i <= SAW ? 225280 : i == LFO ? 6144 : i == CARRIER ? 133120 : 44100;
        zones[i] = (afx_c_zone_t){
            .sample = {(const uint8_t *)samples[i], sizeof(samples[i]), 2048, AFX_PCM16, 69, i != CLICK, 0, 2047, 0, rate},
            .key_max = 127, .velocity_max = 127,
            .dsp_send = 0xf0 | (i < LFO ? 0 : i <= CARRIER ? 1 : i-RAMP0+1),
            .setup_mask = 1u << AFX_FIELD_DIRECT,
            .setup = {[AFX_FIELD_DIRECT] = i < LFO ? 0xf00 : 0},
        };
    }
    const char *names[] = {"click.afx", "drive.afx", "saw.afx", "lfo.afx", "carrier.afx", "pitch.afx"};
    for (unsigned input = 0; input < 6; ++input) {
        afx_c_note_t notes[5] = {0};
        unsigned count = input < 3 ? 1 : input < 5 ? 2 : 5;
        for (unsigned n = 0; n < count; ++n)
            notes[n] = (afx_c_note_t){.start_tick=250, .release_tick=2750, .end_tick=4500,
                .key=69, .velocity=127, .setup_index = 1 + (n ? (input == 3 ? LFO : input == 4 ? CARRIER : RAMP0+n-1) : input < 3 ? input : SAW)};
        afx_c_output_t out;
        assert(!afx_c_compile_zones(notes, count, 1000, zones, COUNT, &out));
        if (!input) save(argv[1], "inputs.afb", out.afb, out.afb_bytes);
        save(argv[1], names[input], out.afx, out.afx_bytes);
        afx_c_output_free(&out);
    }
}
