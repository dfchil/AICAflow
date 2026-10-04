/* Offline AICA DSP authoring tool. Runtime and offline use dsp_prefabs.c. */
#include <aicaflow/dsp.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int usage(const char *program) {
    fprintf(stderr, "usage: %s [--demo] PRESET OUTPUT.dsp\n", program);
    return 2;
}

static int make_parent_directories(const char *output) {
    char path[4096];
    size_t length = strlen(output);
    if (length >= sizeof(path)) return -1;
    memcpy(path, output, length + 1);
    for (char *cursor = path + 1; *cursor; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = 0;
        if (mkdir(path, 0777) && errno != EEXIST) return -1;
        *cursor = '/';
    }
    return 0;
}

int main(int argc, char **argv) {
    int demo = argc == 4 && !strcmp(argv[1], "--demo");
    if (argc != 3 && !demo) return usage(argv[0]);
    const char *preset = demo ? argv[2] : argv[1];
    const char *output_path = demo ? argv[3] : argv[2];
    const char *description = afx_dsp_program_preset_description(preset);
    if (!description) return usage(argv[0]);
    FILE *existing = fopen(output_path, "rb");
    if (existing) { fclose(existing); fprintf(stderr, "%s: exists\n", output_path); return 1; }
    if (errno != ENOENT) { perror(output_path); return 1; }
    if (make_parent_directories(output_path)) { perror(output_path); return 1; }
    afx_dsp_program_t image;
    if ((demo ? afx_dsp_program_demo(&image, preset) : afx_dsp_program_preset(&image, preset))) return 1;
    FILE *output = fopen(output_path, "wb");
    if (!output) { perror(output_path); return 1; }
    int failed = fwrite(&image, 1, sizeof(image), output) != sizeof(image) || fclose(output);
    if (failed) { perror(output_path); return 1; }
    printf("%s: %s%s\n", output_path, description, demo ? " (demo gain)" : "");
    return 0;
}
