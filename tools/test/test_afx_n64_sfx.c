/* Test the private lowering boundary without adding a runtime/public API. */
#define main afx_n64_cli_main
#include "afx_n64.c"
#undef main
#include <assert.h>

static void put_be32(uint8_t *p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}

int main(void) {
    uint8_t control[256] = {'B', '1', 0, 1}, table[256];
    put_be32(control + 4, 8);
    control[9] = 1; put_be32(control + 12, 22050); put_be32(control + 20, 24);
    control[39] = 3;
    for (uint32_t i = 0; i < 3; ++i) {
        uint32_t sound = 64 + i * 16, envelope = 112 + i * 16, keymap = 160 + i * 8;
        put_be32(control + 40 + i * 4, sound);
        put_be32(control + sound, envelope); put_be32(control + sound + 4, keymap);
        put_be32(control + sound + 8, i == 2 ? 232 : 192);
        control[sound + 12] = 64; control[sound + 13] = 127;
        put_be32(control + envelope + 4, i == 0 ? UINT32_MAX : i == 1 ? 100000 : 1000);
        put_be32(control + envelope + 8, i == 2 ? 1000 : 100000);
        control[envelope + 12] = control[envelope + 13] = 127;
        control[keymap + 4] = 60;
    }
    control[160] = 2; control[161] = 1; /* Sustained component then a finite one, 33 ms later. */
    put_be32(control + 196, sizeof(table)); control[200] = 1;
    put_be32(control + 204, 216);
    put_be32(control + 220, 128); put_be32(control + 224, UINT32_MAX);
    put_be32(control + 236, sizeof(table)); control[240] = 1;
    for (uint32_t i = 0; i < 128; ++i) {
        int16_t value = i & 1 ? -32700 : 32700;
        table[2 * i] = (uint16_t)value >> 8; table[2 * i + 1] = (uint8_t)value;
    }
    bank_t bank;
    assert(!bank_open(&bank, control, sizeof(control), table, sizeof(table), 0));
    afx_c_event_t *events = NULL; afx_c_zone_t *zones = NULL;
    uint32_t event_count = 0, duration = 0, zone_count = 0; int park = 0;
    assert(!lower_sfx(&bank, 1, &events, &event_count, &duration, &zones, &zone_count, &park));
    assert(park && zone_count == 2 && event_count == 4 && duration == 133);
    assert(events[2].opcode == AFX_OP_KEYOFF && events[2].channel == 1 && events[2].tick == 133);
    assert(((zones[0].setup[AFX_FIELD_ENV_AD] >> 6) & 31) == 0);
    assert(zones[0].sample.format == AFX_PCM8 && zones[0].sample.sample_rate == 22050);
    for (uint32_t i = 0; i < zone_count; ++i) {
        assert(zones[i].setup[AFX_FIELD_PITCH] == sfx_pitch(22050, 0));
        assert(zones[i].setup[AFX_FIELD_PITCH] == events[i].fields[AFX_FIELD_PITCH]);
        assert(zones[i].setup[AFX_FIELD_TOTAL_LEVEL] == events[i].fields[AFX_FIELD_TOTAL_LEVEL]);
    }
    afx_c_output_t output = {0};
    assert(!afx_c_compile_events(events, event_count, duration, 1000, zones, zone_count, &output));
    assert(!afx_file_validate(output.afx, output.afx_bytes, NULL));
    assert(afx_read32(output.afx + 12) == AFX_FLAG_CONTROLLED && !output.afc && !output.afv);
    assert(afx_read16(output.afx + afx_read32(output.afx + 16) + 2 * AFX_FIELD_PITCH) == sfx_pitch(22050, 0));
    afx_c_output_free(&output);
    assert(zones[0].sample.data == zones[1].sample.data);
    free((void *)zones[0].sample.data); free(zones); free(events);

    control[160] = 0; event_count = zone_count = duration = 0; park = 0;
    assert(!lower_sfx(&bank, 1, &events, &event_count, &duration, &zones, &zone_count, &park));
    assert(park && event_count == 2 && duration == 0);
    assert(!afx_c_compile_events(events, event_count, duration, 1000, zones, zone_count, &output));
    assert(output.afx[output.afx_bytes - 1] == AFX_OP_PARK);
    afx_c_output_free(&output);
    free((void *)zones[0].sample.data); free(zones); free(events);

    event_count = zone_count = duration = 0; park = 0;
    assert(!lower_sfx(&bank, 3, &events, &event_count, &duration, &zones, &zone_count, &park));
    assert(!park && event_count == 2 && duration == 6); /* 128 frames / 22050, not / 44100. */
    free((void *)zones[0].sample.data); free(zones); free(events);

    uint8_t quiet[] = {1, 0, 255, 255}, *encoded = NULL, format;
    uint32_t bytes, frames;
    assert(!afx_c_encode_sample_at_rate(quiet, 2, 22050, 22050, 1, 30, 24, &encoded, &bytes, &format, &frames));
    assert(format == AFX_PCM16 && bytes == sizeof(quiet) && !memcmp(encoded, quiet, bytes));
    free(encoded);
    /* A truncated VADPCM codebook must be rejected before reading its coefficients. */
    control[200] = 0; put_be32(control + 208, 248);
    put_be32(control + 248, 2); put_be32(control + 252, 1);
    int loop; uint32_t first, end;
    assert(make_pcm16(&bank, 192, &encoded, &frames, &first, &end, &loop));
    return 0;
}
