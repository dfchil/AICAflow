#include "afx_compile_c.h"
#include "afx_midi_c.h"
#include "afx_sample_c.h"
#include "afx_sf2_c.h"

#include <aicaflow/codec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_file(const char *path, const uint8_t *data, uint32_t bytes) {
    FILE *file = fopen(path, "wb");
    int failed = !file || fwrite(data, 1, bytes, file) != bytes;
    if (file && fclose(file)) failed = 1;
    return failed ? -1 : 0;
}

static int sibling_path(const char *afx_path, const char *extension, char path[4096]) {
    size_t length = strlen(afx_path);
    if (length < 4 || strcmp(afx_path + length - 4, ".afx") || length - 4 + strlen(extension) >= 4096)
        return -1;
    memcpy(path, afx_path, length - 4);
    strcpy(path + length - 4, extension);
    return 0;
}

static uint8_t *read_file(const char *path, uint32_t *out_bytes) {
    FILE *in = fopen(path, "rb");
    long size;
    uint8_t *data;
    if (!in || fseek(in, 0, SEEK_END) || (size = ftell(in)) < 0 ||
        (uint64_t)size > UINT32_MAX || fseek(in, 0, SEEK_SET)) goto failed;
    data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, in) != (size_t)size) { free(data); goto failed; }
    fclose(in); *out_bytes = (uint32_t)size; return data;
failed:
    if (in) fclose(in);
    return NULL;
}

static void free_zones(afx_c_zone_t *zones, uint8_t **owned, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) free(owned[i]);
    free(owned); free(zones);
}

static int load_zones(const char *path, afx_c_zone_t **out_zones, uint8_t ***out_owned,
                      uint32_t *out_count) {
    FILE *file = fopen(path, "r");
    afx_c_zone_t *zones = NULL; uint8_t **owned = NULL;
    uint32_t count = 0, capacity = 0; char line[1200];
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) {
        unsigned key_min, key_max, root; int loop_start, loop_end;
        char format_name[16], sample_path[1024];
        char *first = line;
        while (*first == ' ' || *first == '\t') ++first;
        int fields = sscanf(line, " %u %u %u %d %d %15s %1023s", &key_min, &key_max, &root,
                            &loop_start, &loop_end, format_name, sample_path);
        if (!fields && (*first == '#' || *first == '\n' || !*first)) continue;
        uint8_t requested_format;
        if (fields != 7 || key_min > key_max || key_max > 127 || root > 127 ||
            afx_c_parse_sample_format(format_name, &requested_format)) goto failed;
        if (count == capacity) {
            uint32_t next = capacity ? capacity * 2u : 8u;
            afx_c_zone_t *new_zones = realloc(zones, (size_t)next * sizeof(*zones));
            if (!new_zones) goto failed;
            zones = new_zones;
            uint8_t **new_owned = realloc(owned, (size_t)next * sizeof(*owned));
            if (!new_owned) goto failed;
            owned = new_owned; capacity = next;
        }
        uint32_t pcm_bytes = 0, encoded_bytes = 0; uint8_t *pcm = read_file(sample_path, &pcm_bytes), *encoded = NULL,
                 format = 0;
        int looping = 0;
        if (!pcm || !pcm_bytes || (pcm_bytes & 1) || pcm_bytes / 2 > 65535) { free(pcm); goto failed; }
        if (loop_start == -1 && loop_end == -1) { }
        else if (loop_start >= 0 && loop_end >= loop_start && (uint32_t)loop_end < pcm_bytes / 2) looping = 1;
        else { free(pcm); goto failed; }
        if (afx_c_encode_sample(pcm, pcm_bytes / 2, looping, requested_format, &encoded, &encoded_bytes, &format)) {
            free(pcm); free(encoded); goto failed;
        }
        free(pcm);
        afx_c_sample_t sample = {encoded, encoded_bytes, pcm_bytes / 2, format, (uint8_t)root,
                                 0, 0, (uint16_t)(pcm_bytes / 2 - 1), 0, 44100};
        if (looping) {
            sample.loop = 1; sample.loop_start = (uint16_t)loop_start; sample.loop_end = (uint16_t)loop_end;
        }
        zones[count] = (afx_c_zone_t){.sample = sample, .key_min = (uint8_t)key_min,
                                      .key_max = (uint8_t)key_max, .velocity_max = 127};
        owned[count++] = encoded;
    }
    fclose(file);
    if (!count) { free_zones(zones, owned, count); return -1; }
    *out_zones = zones; *out_owned = owned; *out_count = count; return 0;
failed:
    fclose(file); free_zones(zones, owned, count); return -1;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--visual")) {
        uint32_t bytes = 0, visual_bytes = 0;
        uint8_t *afx = read_file(argv[2], &bytes), *visual = NULL;
        int result = !afx || afx_c_visualize(afx, bytes, &visual, &visual_bytes) ||
                     write_file(argv[3], visual, visual_bytes);
        free(afx); free(visual);
        return result ? fprintf(stderr, "cannot generate AFX visualizer\n"), 1 : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--summary")) {
        uint32_t afb_bytes = 0, afx_bytes = 0, notes = 0;
        afx_file_header_t header;
        uint8_t *afb = read_file(argv[2], &afb_bytes), *afx = read_file(argv[3], &afx_bytes);
        if (!afb || !afx || afb_bytes < AFX_BANK_HEADER_BYTES || afx_bytes < AFX_FILE_HEADER_BYTES ||
            afx_read32(afb) != AFX_BANK_MAGIC || afx_file_validate(afx, afx_bytes, &header)) {
            free(afb); free(afx); return fprintf(stderr, "cannot summarize AFB/AFX pair\n"), 2;
        }
        const uint8_t *image = afx + header.image_offset;
        for (uint32_t offset = header.stream_offset, end = header.stream_offset + header.stream_size; offset < end;) {
            afx_event_t event;
            if (afx_decode_event(image + offset, end - offset, &event)) {
                free(afb); free(afx); return fprintf(stderr, "cannot summarize AFB/AFX pair\n"), 2;
            }
            if (event.opcode == AFX_OP_NOTE) ++notes;
            offset += event.bytes;
        }
        printf("%u %u %u %u %u %u %u\n", afb_bytes + afx_bytes,
               afb_bytes - AFX_BANK_HEADER_BYTES, afx_read32(afx + 28),
               afx_read32(afx + 24), notes, afx_read32(afx + 52), afx_read32(afx + 64));
        free(afb); free(afx); return 0;
    }
    int zones_mode = argc == 6 && !strcmp(argv[2], "--zones");
    int sf2_mode = argc == 7 && !strcmp(argv[2], "--sf2");
    if ((argc != 4 && argc != 5) && !zones_mode && !sf2_mode)
        return fprintf(stderr, "usage: %s --visual flow.afx out.afv\n"
                       "       %s source.mid [sample.pcm] out.afb out.afx\n"
                       "       %s source.mid --zones zones.txt out.afb out.afx\n"
                       "       %s source.mid --sf2 pcm16|pcm8|adpcm|auto bank.sf2 out.afb out.afx\n", argv[0], argv[0], argv[0], argv[0]), 2;
    uint32_t midi_bytes;
    uint8_t *data = read_file(argv[1], &midi_bytes);
    if (!data) return fprintf(stderr, "cannot read %s\n", argv[1]), 2;
    afx_c_note_t *notes = NULL; uint32_t count = 0;
    int parsed = afx_c_midi_notes(data, midi_bytes, 1000, &notes, &count);
    free(data);
    afx_c_output_t out;
    uint8_t *pcm = NULL; uint32_t pcm_bytes = 0;
    if (!parsed && argc == 5) pcm = read_file(argv[2], &pcm_bytes);
    afx_c_sample_t sample = {pcm, pcm_bytes, pcm_bytes / 2, AFX_PCM16, 69,
                             0, 0, pcm_bytes / 2 ? pcm_bytes / 2 - 1 : 0, 0, 44100};
    afx_c_zone_t *zones = NULL; uint8_t **owned = NULL; uint32_t zone_count = 0;
    afx_c_sf2_output_t sf2 = {0};
    if (!parsed && zones_mode && load_zones(argv[3], &zones, &owned, &zone_count)) parsed = -1;
    afx_c_sf2_options_t sf2_options;
    afx_c_sf2_options_default(&sf2_options);
    if (!parsed && sf2_mode && (afx_c_parse_sample_format(argv[3], &sf2_options.sample_format) ||
                                afx_c_sf2_resolve(argv[4], notes, count, &sf2_options, &sf2))) parsed = -1;
    int result = parsed || (argc == 5 && (!pcm || (pcm_bytes & 1))) ? -1 :
                 sf2_mode ? afx_c_compile_zones(sf2.notes, sf2.note_count, 1000,
                                                 sf2.zones, sf2.zone_count, &out) :
                 zones_mode ? afx_c_compile_zones(notes, count, 1000, zones, zone_count, &out) :
                 argc == 5 ? afx_c_compile_sample(notes, count, 1000, &sample, &out) :
                 afx_c_compile_sine(notes, count, 1000, &out);
    free(notes);
    free(pcm);
    free_zones(zones, owned, zone_count);
    afx_c_sf2_output_free(&sf2);
    if (result) return fprintf(stderr, "cannot compile source\n"), 1;
    if (out.afb_bytes < AFX_BANK_HEADER_BYTES || out.afb_bytes - AFX_BANK_HEADER_BYTES > AFX_TARGET_MAX_BANK_BYTES) {
        afx_c_output_free(&out);
        return fprintf(stderr, "AFB payload exceeds the AICA asset arena\n"), 1;
    }
    char afc_path[4096], afv_path[4096];
    result = sibling_path(argv[argc - 1], ".afc", afc_path) ||
             sibling_path(argv[argc - 1], ".afv", afv_path) ||
             write_file(argv[argc - 2], out.afb, out.afb_bytes) ||
             write_file(argv[argc - 1], out.afx, out.afx_bytes) ||
             write_file(afc_path, out.afc, out.afc_bytes) ||
             write_file(afv_path, out.afv, out.afv_bytes);
    afx_c_output_free(&out);
    return result ? 1 : 0;
}
