/* Plays a flow through the host backend and writes what it renders as a WAV:
 * the real firmware and SH4 library on the software AICA.
 *
 *   afx_render [-s seconds] [-g gain] [-f firmware.drv] [--room feedback,damping,wet_q8]
 *              [--coef index=value]... bank.afb flow.afx out.wav
 *
 * --room installs afx_dsp_program_room() as the DSP scene; --coef then
 * overrides its coefficients, as a program tuning a preset would. */
#include <aicaflow/bank.h>
#include <aicaflow/dsp.h>
#include <aicaflow/host.h>
#include <aicaflow/host_audio.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *read_file(const char *path, uint32_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void *data = length > 0 ? aligned_alloc(32, ((size_t)length + 31) & ~(size_t)31) : NULL;
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) { free(data); data = NULL; }
    fclose(file);
    *size = (uint32_t)length;
    return data;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static volatile int ended;
static void on_end(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    (void)instance; (void)status; (void)user;
    ended = 1;
}

int main(int argc, char **argv) {
    const char *firmware_path = AFX_RENDER_FIRMWARE;
    double seconds = 30;
    int gain = 255;
    bool room = false;
    int room_feedback = 0, room_damping = 0, room_wet = 0;
    int coef_index[64], coef_value[64], coefs = 0;
    int arg = 1;
    for (; arg < argc && argv[arg][0] == '-'; ++arg) {
        if (!strcmp(argv[arg], "-s") && arg + 1 < argc) seconds = atof(argv[++arg]);
        else if (!strcmp(argv[arg], "-g") && arg + 1 < argc) gain = atoi(argv[++arg]);
        else if (!strcmp(argv[arg], "-f") && arg + 1 < argc) firmware_path = argv[++arg];
        else if (!strcmp(argv[arg], "--room") && arg + 1 < argc &&
                 sscanf(argv[++arg], "%d,%d,%d", &room_feedback, &room_damping, &room_wet) == 3) room = true;
        else if (!strcmp(argv[arg], "--coef") && arg + 1 < argc && coefs < 64 &&
                 sscanf(argv[++arg], "%d=%d", &coef_index[coefs], &coef_value[coefs]) == 2) ++coefs;
        else { fprintf(stderr, "afx_render: bad option %s\n", argv[arg]); return 2; }
    }
    if (argc - arg != 3) {
        fprintf(stderr, "usage: afx_render [-s seconds] [-g gain] [-f firmware.drv] [--room fb,damping,wet_q8] "
                        "[--coef index=value]... bank.afb flow.afx out.wav\n");
        return 2;
    }
    uint32_t firmware_size, flow_size;
    void *firmware = read_file(firmware_path, &firmware_size);
    void *flow_data = read_file(argv[arg + 1], &flow_size);
    if (!firmware || !flow_data) { fprintf(stderr, "afx_render: cannot read the firmware or flow\n"); return 1; }
    int result = afx_init(firmware, firmware_size);
    if (result) { fprintf(stderr, "afx_render: afx_init %d\n", result); return 1; }
    if (room) {
        afx_dsp_program_t program;
        result = afx_dsp_program_room(&program, (int16_t)room_feedback, (int16_t)room_damping, false,
                                      (int16_t)room_wet, false);
        for (int i = 0; !result && i < coefs; ++i)
            result = afx_dsp_program_coefficient(&program, (uint8_t)coef_index[i], (int16_t)(coef_value[i] & ~7));
        if (!result) result = afx_dsp_scene_program(&program, sizeof(program));
        if (result) { fprintf(stderr, "afx_render: room %d\n", result); return 1; }
    }
    afx_bank_t bank = {0};
    afx_asset_t flow;
    afx_instance_t instance;
    result = afx_bank_load_file(&bank, argv[arg]);
    if (!result) result = afx_bank_flow_upload(&bank, flow_data, flow_size, &flow);
    afx_set_end_callback(on_end, NULL);
    if (!result) result = afx_instance_activate(flow, &instance);
    if (!result && gain != 255) result = afx_instance_gain(instance, (uint8_t)gain);
    if (result) { fprintf(stderr, "afx_render: load/activate %d\n", result); return 1; }

    FILE *out = fopen(argv[arg + 2], "wb");
    if (!out) { fprintf(stderr, "afx_render: cannot write %s\n", argv[arg + 2]); return 1; }
    uint8_t header[44] = {0};
    fwrite(header, 1, sizeof(header), out);
    uint32_t rate = afx_host_audio_rate(), total = (uint32_t)(seconds * rate), frames = 0;
    int16_t buffer[2 * 1024];
    long peak = 0, clipped = 0;
    while (frames < total && !ended) {
        uint32_t n = total - frames < 1024 ? total - frames : 1024;
        afx_host_audio_render(buffer, n);
        for (uint32_t i = 0; i < 2 * n; ++i) {
            long v = labs((long)buffer[i]);
            if (v > peak) peak = v;
            if (v >= 32767) ++clipped;
        }
        fwrite(buffer, 4, n, out);
        frames += n;
    }
    memcpy(header, "RIFF", 4); put32(header + 4, 36 + frames * 4);
    memcpy(header + 8, "WAVEfmt ", 8); put32(header + 16, 16); put16(header + 20, 1); put16(header + 22, 2);
    put32(header + 24, rate); put32(header + 28, rate * 4); put16(header + 32, 4); put16(header + 34, 16);
    memcpy(header + 36, "data", 4); put32(header + 40, frames * 4);
    fseek(out, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), out);
    fclose(out);
    printf("%s: %.1f s%s, peak %ld, clipped samples %ld\n", argv[arg + 2], (double)frames / rate,
           ended ? " (the flow ended)" : "", peak, clipped);
    if (!ended) (void)afx_instance_stop(instance);
    afx_shutdown();
    return 0;
}
