/* The test clock: every sleep renders audio, which is what advances the AICA.
 * With AFX_HOST_WAV=path the rendered audio is also written as a WAV file. */
#include "kos.h"

#include <aicaflow/host_audio.h>

#include <stdlib.h>
#include <string.h>

void afx_host_settle(void);

static uint64_t g_frames;
static uint32_t g_fraction;
static FILE *g_wav;
static uint32_t g_wav_frames;

static void wav_header(FILE *file, uint32_t frames) {
    uint32_t bytes = frames * 4u, rate = afx_host_audio_rate();
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36u + bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t s = 1; memcpy(h + 20, &s, 2);
    s = 2; memcpy(h + 22, &s, 2);
    memcpy(h + 24, &rate, 4);
    v = rate * 4u; memcpy(h + 28, &v, 4);
    s = 4; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fseek(file, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), file);
}

static void wav_close(void) {
    if (!g_wav) return;
    wav_header(g_wav, g_wav_frames);
    fclose(g_wav);
    g_wav = NULL;
}

static void render(uint32_t frames) {
    static int16_t buffer[2 * 1024];
    static int opened;
    if (!opened) {
        opened = 1;
        const char *path = getenv("AFX_HOST_WAV");
        if (path && *path && (g_wav = fopen(path, "wb"))) {
            wav_header(g_wav, 0);
            atexit(wav_close);
        }
    }
    while (frames) {
        uint32_t n = frames < 64 ? frames : 64;
        afx_host_audio_render(buffer, n);
        afx_host_settle();
        if (g_wav) { fwrite(buffer, 4, n, g_wav); g_wav_frames += n; }
        g_frames += n;
        frames -= n;
    }
}

uint64_t afx_test_ms(void) { return g_frames * 1000u / afx_host_audio_rate(); }

void afx_test_sleep(uint32_t milliseconds) {
    uint32_t total = milliseconds * afx_host_audio_rate() + g_fraction;
    g_fraction = total % 1000u;
    render(total / 1000u);
}

void afx_test_pass(void) { render(16); }
