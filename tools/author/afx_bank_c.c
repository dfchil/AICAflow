/* realpath and PATH_MAX are POSIX interfaces, not ISO C11. */
#define _XOPEN_SOURCE 700
#include "afx_compile_c.h"
#include "afx_midi_c.h"
#include "afx_sample_c.h"
#include "afx_sf2_c.h"

#include <aicaflow/codec.h>

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[128], path[1024]; uint8_t channel; } source_t;
typedef struct {
    char song[128];
    unsigned midi_bank, midi_program, sf2_bank, sf2_program;
    int midi_channel;
    afx_c_sf2_options_t options;
    source_t *source;
} map_t;
typedef struct { bool enabled; uint32_t seed; uint8_t level_centibels, shorten_ms, lfo_rate_steps; } humanize_t;
typedef struct {
    char name[128], midi[1024];
    uint32_t tick_rate, release_tail_ms;
    humanize_t humanize;
    afx_c_sf2_output_t resolved;
} song_t;

/* `humanize` hashes a fixed, short JSON identity.  Keeping this tiny SHA-256
 * implementation local avoids an authoring-time dependency while matching the
 * original expression_sha256_v1 byte-for-byte. */
static const uint32_t sha256_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
static uint32_t ror32(uint32_t value, unsigned bits) { return value >> bits | value << (32u - bits); }
static void sha256_identity(const char *text, uint8_t out[32]) {
    uint8_t block[64] = {0}; uint32_t words[64], state[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    size_t bytes = strlen(text);
    memcpy(block, text, bytes); block[bytes] = 0x80;
    for (unsigned i = 0; i < 8; ++i)
        block[56 + i] = (uint8_t)((uint64_t)bytes * 8u >> (56 - 8 * i));
    for (unsigned i = 0; i < 16; ++i)
        words[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
                   (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = ror32(words[i - 15], 7) ^ ror32(words[i - 15], 18) ^ words[i - 15] >> 3;
        uint32_t s1 = ror32(words[i - 2], 17) ^ ror32(words[i - 2], 19) ^ words[i - 2] >> 10;
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a=state[0], b=state[1], c=state[2], d=state[3], e=state[4], f=state[5], g=state[6], h=state[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1=ror32(e,6)^ror32(e,11)^ror32(e,25), choose=(e&f)^(~e&g);
        uint32_t s0=ror32(a,2)^ror32(a,13)^ror32(a,22), majority=(a&b)^(a&c)^(b&c);
        uint32_t next1=h+s1+choose+sha256_k[i]+words[i], next2=s0+majority;
        h=g; g=f; f=e; e=d+next1; d=c; c=b; b=a; a=next1+next2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
    for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 4; ++j)
        out[i * 4 + j] = (uint8_t)(state[i] >> (24 - 8 * j));
}

static uint32_t little32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static int humanize_notes(afx_c_note_t *notes, uint32_t count, uint32_t tick_rate, const humanize_t *settings) {
    if (!settings->enabled) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        char identity[64]; uint8_t digest[32]; uint32_t duration, maximum;
        int bytes = snprintf(identity, sizeof(identity), "[%u,%u,%u,%u,%u]", settings->seed,
                             notes[i].source_id, notes[i].source_track, notes[i].source_tick, notes[i].key);
        if (bytes < 0 || (size_t)bytes >= sizeof(identity) || notes[i].end_tick <= notes[i].start_tick) return -1;
        sha256_identity(identity, digest);
        notes[i].attenuation_offset_centibels = (int16_t)(little32(digest) % (2u * settings->level_centibels + 1u)) -
                                                 settings->level_centibels;
        duration = notes[i].end_tick - notes[i].start_tick;
        maximum = (settings->shorten_ms * tick_rate + 500u) / 1000u;
        if (maximum > duration / 20u) maximum = duration / 20u;
        notes[i].end_tick -= little32(digest + 4) % (maximum + 1u);
        notes[i].release_tick = notes[i].end_tick;
    }
    return 0;
}

static int read_file(const char *path, uint8_t **out_data, uint32_t *out_bytes) {
    FILE *file = fopen(path, "rb"); long bytes; uint8_t *data;
    if (!file || fseek(file, 0, SEEK_END) || (bytes = ftell(file)) < 1 ||
        (uint64_t)bytes > UINT32_MAX || fseek(file, 0, SEEK_SET)) goto failed;
    data = malloc((size_t)bytes);
    if (!data || fread(data, 1, (size_t)bytes, file) != (size_t)bytes) { free(data); goto failed; }
    fclose(file); *out_data = data; *out_bytes = (uint32_t)bytes; return 0;
failed:
    if (file) fclose(file); return -1;
}

static int write_file(const char *path, const uint8_t *data, uint32_t bytes) {
    FILE *file = fopen(path, "wb");
    int failed = !file || fwrite(data, 1, bytes, file) != bytes;
    if (file && fclose(file)) failed = 1;
    return failed ? -1 : 0;
}

static int relative_path(const char *map_path, const char *path, char out[1200]) {
    const char *slash = strrchr(map_path, '/');
    size_t dir = slash ? (size_t)(slash - map_path) : 0;
    if (path[0] == '/' || !dir) return snprintf(out, 1200, "%s", path) >= 1200 ? -1 : 0;
    if (dir + 1u + strlen(path) >= 1200) return -1;
    memcpy(out, map_path, dir); out[dir] = '/'; strcpy(out + dir + 1u, path); return 0;
}

static int map_relative_path(const char *map_path, const char *input, char out[1200]) {
    char map_copy[PATH_MAX], map_dir[PATH_MAX], target[PATH_MAX];
    const char *slash = strrchr(map_path, '/');
    if (slash) {
        size_t bytes = (size_t)(slash - map_path);
        if (bytes >= sizeof(map_copy)) return -1;
        memcpy(map_copy, map_path, bytes); map_copy[bytes] = 0;
    } else strcpy(map_copy, ".");
    if (!realpath(map_copy, map_dir) || !realpath(input, target)) return -1;
    size_t common = 0, boundary = 0;
    while (map_dir[common] && map_dir[common] == target[common]) {
        if (map_dir[common] == '/') boundary = common + 1u;
        ++common;
    }
    const char *from = map_dir + boundary, *to = target + boundary;
    size_t used = 0;
    while (*from) {
        const char *next = strchr(from, '/');
        if (used + 3 >= 1200) return -1;
        memcpy(out + used, "../", 3); used += 3;
        from = next ? next + 1 : from + strlen(from);
    }
    if (!*to && !used) { strcpy(out, "."); return 0; }
    if (used + strlen(to) >= 1200) return -1;
    strcpy(out + used, to); return 0;
}

static int flow_paths(const char *directory, const char *name, char afx[4096], char afc[4096], char afv[4096]) {
    return snprintf(afx, 4096, "%s/%s.afx", directory, name) >= 4096 ||
           snprintf(afc, 4096, "%s/%s.afc", directory, name) >= 4096 ||
           snprintf(afv, 4096, "%s/%s.afv", directory, name) >= 4096 ? -1 : 0;
}

static uint32_t align32(uint32_t value) { return (value + 31u) & ~31u; }

static int index_paths(const char *afb, char index[4096], char named[4096]) {
    size_t bytes = strlen(afb);
    if (bytes < 4 || strcmp(afb + bytes - 4, ".afb") || bytes - 4u + 11u >= 4096) return -1;
    return snprintf(index, 4096, "%.*s.afi", (int)(bytes - 4u), afb) >= 4096 ||
           snprintf(named, 4096, "%.*s.names.afi", (int)(bytes - 4u), afb) >= 4096 ? -1 : 0;
}

static int write_afi(const char *path, const uint8_t *afb, uint32_t afb_bytes,
                     const afx_c_zone_t *source, uint32_t source_count,
                     const char (*zone_names)[AFX_C_SAMPLE_NAME_BYTES], int names) {
    uint32_t afb_payload, *offsets = NULL, *zones = NULL;
    if (!path || !afb || !source || !source_count || afb_bytes < AFX_BANK_HEADER_BYTES ||
        afx_read32(afb) != AFX_BANK_MAGIC) return -1;
    afb_payload = afx_read32(afb + 16);
    if (afb_payload != AFX_BANK_HEADER_BYTES) return -1;
    offsets = malloc((size_t)source_count * sizeof(*offsets));
    zones = malloc((size_t)source_count * sizeof(*zones));
    if (!offsets || !zones) goto failed;
    uint32_t count = 0;
    uint32_t bytes = 0;
    for (uint32_t zone = 0; zone < source_count; ++zone) {
        uint32_t sample = 0;
        while (sample < count && (source[zone].sample.data != source[zones[sample]].sample.data ||
                                  source[zone].sample.bytes != source[zones[sample]].sample.bytes)) ++sample;
        if (sample != count) continue;
        bytes = align32(bytes);
        if (!source[zone].sample.bytes || bytes > afx_read32(afb + 20) ||
            source[zone].sample.bytes > afx_read32(afb + 20) - bytes) goto failed;
        offsets[count] = bytes; zones[count++] = zone;
        if (source[zone].sample.bytes > UINT32_MAX - bytes) goto failed;
        bytes += source[zone].sample.bytes;
    }
    uint32_t record_bytes = names ? AFX_INDEX_NAMED_RECORD_BYTES : AFX_INDEX_RECORD_BYTES;
    uint64_t total = (uint64_t)AFX_INDEX_HEADER_BYTES + (uint64_t)count * record_bytes;
    if (total > UINT32_MAX - 31u) goto failed;
    uint32_t file_bytes = align32((uint32_t)total);
    uint8_t *data = calloc(1, file_bytes);
    if (!data) goto failed;
    afx_write32(data, AFX_INDEX_MAGIC); afx_write32(data + 4, AFX_INDEX_VERSION);
    afx_write32(data + 8, afx_read32(afb + 8)); afx_write32(data + 12, afx_read32(afb + 12));
    afx_write32(data + 16, AFX_INDEX_HEADER_BYTES); afx_write32(data + 20, count);
    afx_write32(data + 24, record_bytes); afx_write32(data + 28, file_bytes);
    uint32_t cursor = AFX_INDEX_HEADER_BYTES;
    for (uint32_t sample = 0; sample < count; ++sample) {
        const afx_c_sample_t *entry = &source[zones[sample]].sample;
        afx_write32(data + cursor, afb_payload + offsets[sample]);
        afx_write32(data + cursor + 4, entry->sample_rate ? entry->sample_rate : 44100u);
        afx_write32(data + cursor + 8, entry->frames);
        data[cursor + 12] = entry->format;
        if (names) {
            char fallback[16] = {0};
            const char *name = zone_names && zone_names[zones[sample]][0] ? zone_names[zones[sample]] :
                               (snprintf(fallback, sizeof(fallback), "sample-%03u", sample), fallback);
            memcpy(data + cursor + AFX_INDEX_RECORD_BYTES, name,
                   strlen(name) < 16u ? strlen(name) : 16u);
        }
        cursor += record_bytes;
    }
    int error = write_file(path, data, file_bytes);
    free(data); free(offsets); free(zones); return error ? -1 : 0;
failed:
    free(offsets); free(zones); return -1;
}

static void free_songs(song_t *songs, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) afx_c_sf2_output_free(&songs[i].resolved);
    free(songs);
}

static source_t *find_source(source_t *sources, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; ++i) if (!strcmp(sources[i].name, name)) return sources + i;
    return NULL;
}

static int parse_filter_model(const char *text, uint8_t *out) {
    if (!strcmp(text, "envelope")) *out = AFX_C_SF2_FILTER_ENVELOPE;
    else if (!strcmp(text, "static")) *out = AFX_C_SF2_FILTER_STATIC;
    else if (!strcmp(text, "none")) *out = AFX_C_SF2_FILTER_NONE;
    else return -1;
    return 0;
}

static int parse_gain_model(const char *text, uint8_t *out) {
    if (!strcmp(text, "standard")) *out = AFX_C_SF2_GAIN_STANDARD;
    else if (!strcmp(text, "fluidsynth2")) *out = AFX_C_SF2_GAIN_FLUIDSYNTH2;
    else return -1;
    return 0;
}

static int parse_unsigned(const char *text, unsigned maximum, unsigned *out) {
    char *end; unsigned long value;
    if (!text || !*text) return -1;
    value = strtoul(text, &end, 10);
    if (*end || value > maximum) return -1;
    *out = (unsigned)value; return 0;
}

static int parse_signed(const char *text, int minimum, int maximum, int *out) {
    char *end; long value;
    if (!text || !*text) return -1;
    value = strtol(text, &end, 10);
    if (*end || value < minimum || value > maximum) return -1;
    *out = (int)value; return 0;
}

static int parse_switch(const char *text, uint8_t *out) {
    if (!strcmp(text, "apply")) *out = 1;
    else if (!strcmp(text, "ignore")) *out = 0;
    else return -1;
    return 0;
}

static int parse_lfo(const char *text, uint16_t *out) {
    unsigned rate, pitch, amplitude; int used = 0;
    if (sscanf(text, "%u,%u,%u%n", &rate, &pitch, &amplitude, &used) != 3 || text[used] ||
        rate > 31 || pitch > 7 || amplitude > 7) return -1;
    *out = (uint16_t)(rate << 10 | pitch << 5 | amplitude); return 0;
}

static int parse_map_option(map_t *item, const char *token) {
    const char *value = strchr(token, '='); unsigned number; int signed_number;
    if (!value || value == token || !value[1]) return -1;
    ++value;
    if (!strncmp(token, "midi_channel=", 13)) {
        if (item->midi_channel >= 0 || parse_unsigned(value, 15, &number)) return -1;
        item->midi_channel = (int)number;
    } else if (!strncmp(token, "rate=", 5)) {
        if (parse_unsigned(value, 192000, &number)) return -1;
        item->options.sample_rate_cap = number;
    } else if (!strncmp(token, "filter_offset=", 14)) {
        if (parse_signed(value, -16000, 16000, &signed_number)) return -1;
        item->options.filter_offset_cents = signed_number;
    } else if (!strncmp(token, "filter=", 7)) {
        if (parse_filter_model(value, &item->options.filter_model)) return -1;
    } else if (!strncmp(token, "gain=", 5)) {
        if (parse_gain_model(value, &item->options.gain_model)) return -1;
    } else if (!strncmp(token, "gain_bias=", 10)) {
        if (parse_signed(value, -10200, 10200, &signed_number)) return -1;
        item->options.gain_bias_centibels = signed_number;
    } else if (!strncmp(token, "envelope=", 9)) {
        if (!strcmp(value, "sf2")) item->options.envelope_model = AFX_C_SF2_ENVELOPE_SF2;
        else if (!strcmp(value, "fixed")) item->options.envelope_model = AFX_C_SF2_ENVELOPE_FIXED;
        else return -1;
    } else if (!strncmp(token, "source_pan=", 11)) {
        if (parse_switch(value, &item->options.source_pan)) return -1;
    } else if (!strncmp(token, "source_reverb=", 14)) {
        if (parse_switch(value, &item->options.source_reverb)) return -1;
    } else if (!strncmp(token, "modulators=", 11)) {
        if (parse_switch(value, &item->options.source_modulators)) return -1;
    } else if (!strncmp(token, "dsp_send=", 9)) {
        if (parse_unsigned(value, 255, &number) || item->options.source_reverb) return -1;
        item->options.has_dsp_send = 1; item->options.dsp_send = (uint8_t)number;
    } else if (!strncmp(token, "direct=", 7)) {
        if (parse_unsigned(value, 65535, &number)) return -1;
        item->options.has_direct = 1; item->options.direct = (uint16_t)number;
    } else if (!strncmp(token, "lfo=", 4)) {
        if (parse_lfo(value, &item->options.lfo)) return -1;
        item->options.has_lfo = 1;
    } else if (!strncmp(token, "loop_ms=", 8)) {
        if (parse_unsigned(value, 2000, &number) || number < 20) return -1;
        item->options.loop_ms = number;
    } else return -1;
    return 0;
}

static map_t *find_map(map_t *maps, uint32_t count, const char *song, const afx_c_note_t *note) {
    unsigned bank = (unsigned)note->bank_msb * 128u + note->bank_lsb;
    unsigned channel = note->controller_state & 15u;
    map_t *song_fallback = NULL, *global_fallback = NULL;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(maps[i].song, song) && maps[i].midi_bank == bank && maps[i].midi_program == note->program) {
            if (maps[i].midi_channel == (int)channel) return maps + i;
            if (maps[i].midi_channel < 0) song_fallback = maps + i;
        }
    if (song_fallback) return song_fallback;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(maps[i].song, "*") && maps[i].midi_bank == bank && maps[i].midi_program == note->program) {
            if (maps[i].midi_channel == (int)channel) return maps + i;
            if (maps[i].midi_channel < 0) global_fallback = maps + i;
        }
    return global_fallback;
}

static int append_resolved(afx_c_sf2_output_t *to, afx_c_sf2_output_t *from) {
    uint32_t old_zones = to->zone_count, old_notes = to->note_count, old_owned = to->owned_count;
    if (!from->zone_names || from->zone_count > UINT16_MAX - old_zones || from->note_count > UINT32_MAX - old_notes ||
        from->owned_count > UINT32_MAX - old_owned) return -1;
    afx_c_zone_t *zones = realloc(to->zones, (size_t)(old_zones + from->zone_count) * sizeof(*zones));
    if (!zones) return -1;
    to->zones = zones;
    char (*names)[AFX_C_SAMPLE_NAME_BYTES] = realloc(to->zone_names,
                                                      (size_t)(old_zones + from->zone_count) * sizeof(*names));
    if (!names) return -1;
    to->zone_names = names;
    afx_c_note_t *notes = realloc(to->notes, (size_t)(old_notes + from->note_count) * sizeof(*notes));
    if (!notes) return -1;
    to->notes = notes;
    uint8_t **owned = realloc(to->owned_samples, (size_t)(old_owned + from->owned_count) * sizeof(*owned));
    if (!owned) return -1;
    to->owned_samples = owned;
    memcpy(to->zones + old_zones, from->zones, (size_t)from->zone_count * sizeof(*to->zones));
    memcpy(to->zone_names + old_zones, from->zone_names,
           (size_t)from->zone_count * sizeof(*to->zone_names));
    for (uint32_t i = 0; i < from->note_count; ++i) {
        if (!from->notes[i].setup_index || from->notes[i].setup_index > UINT16_MAX - old_zones) return -1;
        from->notes[i].setup_index = (uint16_t)(from->notes[i].setup_index + old_zones);
    }
    memcpy(to->notes + old_notes, from->notes, (size_t)from->note_count * sizeof(*to->notes));
    memcpy(to->owned_samples + old_owned, from->owned_samples, (size_t)from->owned_count * sizeof(*to->owned_samples));
    to->zone_count += from->zone_count; to->note_count += from->note_count; to->owned_count += from->owned_count;
    free(from->zones); free(from->zone_names); free(from->notes); free(from->owned_samples);
    *from = (afx_c_sf2_output_t){0};
    return 0;
}

static int parse_map(const char *path, source_t **out_sources, uint32_t *out_source_count,
                     map_t **out_maps, uint32_t *out_map_count, song_t **out_songs, uint32_t *out_song_count) {
    FILE *file = fopen(path, "r"); char line[1400];
    source_t *sources = NULL; map_t *maps = NULL; song_t *songs = NULL;
    uint32_t source_count = 0, map_count = 0, song_count = 0;
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) {
        char kind[16], channel[16];
        char *cursor = line; while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '#' || *cursor == '\n' || !*cursor) continue;
        if (sscanf(cursor, "%15s", kind) != 1) goto failed;
        if (!strcmp(kind, "source")) {
            source_t item = {0}; char resolved[1200];
            int fields = sscanf(cursor, "source %127s \"%1023[^\"]\" %15s", item.name, item.path, channel);
            if (fields >= 2) ++fields; /* Include the literal `source` token. */
            else
                fields = sscanf(cursor, "%15s %127s %1023s %15s", kind, item.name, item.path, channel);
            if (fields < 3 || fields > 4 || (fields == 4 && strcmp(channel, "stereo") &&
                strcmp(channel, "left") && strcmp(channel, "right")) ||
                map_count || song_count || find_source(sources, source_count, item.name) ||
                relative_path(path, item.path, resolved)) goto failed;
            item.channel = fields == 4 && !strcmp(channel, "left") ? AFX_C_SF2_LEFT :
                           fields == 4 && !strcmp(channel, "right") ? AFX_C_SF2_RIGHT : AFX_C_SF2_STEREO;
            strcpy(item.path, resolved);
            source_t *grown = realloc(sources, (size_t)(source_count + 1u) * sizeof(*sources));
            if (!grown) goto failed;
            sources = grown; sources[source_count++] = item;
        } else if (!strcmp(kind, "map")) {
            char copy[sizeof(line)], *fields[32], *token; unsigned midi_bank, midi_program, sf2_bank, sf2_program;
            uint32_t field_count = 0; map_t item = {.midi_channel = -1};
            strcpy(copy, cursor);
            for (token = strtok(copy, " \t\r\n"); token; token = strtok(NULL, " \t\r\n")) {
                if (field_count == sizeof(fields) / sizeof(*fields)) goto failed;
                fields[field_count++] = token;
            }
            afx_c_sf2_options_default(&item.options);
            if (field_count < 8 || song_count || strcmp(fields[0], "map") ||
                strlen(fields[1]) >= sizeof(item.song) || parse_unsigned(fields[2], 16383, &midi_bank) ||
                parse_unsigned(fields[3], 127, &midi_program) || parse_unsigned(fields[5], 16383, &sf2_bank) ||
                parse_unsigned(fields[6], 127, &sf2_program) || afx_c_parse_sample_format(fields[7], &item.options.sample_format) ||
                !(item.source = find_source(sources, source_count, fields[4]))) goto failed;
            strcpy(item.song, fields[1]);
            for (uint32_t field = 8; field < field_count; ++field)
                if (parse_map_option(&item, fields[field])) goto failed;
            if (item.options.source_reverb && item.options.has_dsp_send) goto failed;
            for (uint32_t i = 0; i < map_count; ++i)
                if (!strcmp(maps[i].song, item.song) && maps[i].midi_bank == midi_bank &&
                    maps[i].midi_program == midi_program && maps[i].midi_channel == item.midi_channel)
                    goto failed;
            item.midi_bank = midi_bank; item.midi_program = midi_program;
            item.sf2_bank = sf2_bank; item.sf2_program = sf2_program;
            map_t *grown = realloc(maps, (size_t)(map_count + 1u) * sizeof(*maps));
            if (!grown) goto failed;
            maps = grown; maps[map_count++] = item;
        } else if (!strcmp(kind, "song")) {
            song_t item = {.tick_rate = 1000}; char resolved[1200];
            int fields = sscanf(cursor, "song %127s \"%1023[^\"]\" %u %u", item.name, item.midi,
                                &item.tick_rate, &item.release_tail_ms);
            if (fields < 2) fields = sscanf(cursor, "%15s %127s %1023s %u %u", kind, item.name,
                                             item.midi, &item.tick_rate, &item.release_tail_ms) - 1;
            if ((fields != 2 && fields != 3 && fields != 4) || !item.tick_rate || item.tick_rate > 1000000 ||
                item.release_tail_ms > 10000 ||
                relative_path(path, item.midi, resolved)) goto failed;
            strcpy(item.midi, resolved);
            for (uint32_t i = 0; i < song_count; ++i) if (!strcmp(songs[i].name, item.name)) goto failed;
            song_t *grown = realloc(songs, (size_t)(song_count + 1u) * sizeof(*songs));
            if (!grown) goto failed;
            songs = grown; songs[song_count++] = item;
        } else if (!strcmp(kind, "humanize")) {
            char name[128]; unsigned seed, level, shorten, lfo; int used = 0;
            if (sscanf(cursor, "humanize %127s %u %u %u %u %n", name, &seed, &level, &shorten, &lfo, &used) != 5 ||
                cursor[used] || level > 120u || shorten > 20u || lfo > 1u) goto failed;
            uint32_t song = 0;
            while (song < song_count && strcmp(songs[song].name, name)) ++song;
            if (song == song_count || songs[song].humanize.enabled || lfo) goto failed;
            songs[song].humanize = (humanize_t){true, seed, (uint8_t)level, (uint8_t)shorten, (uint8_t)lfo};
        } else goto failed;
    }
    fclose(file);
    if (!source_count || !map_count || !song_count) goto failed_no_file;
    *out_sources = sources; *out_source_count = source_count; *out_maps = maps; *out_map_count = map_count;
    *out_songs = songs; *out_song_count = song_count; return 0;
failed:
    fclose(file);
failed_no_file:
    free(sources); free(maps); free_songs(songs, song_count); return -1;
}

static int resolve_song(song_t *song, map_t *maps, uint32_t map_count) {
    uint8_t *midi = NULL; uint32_t midi_bytes = 0;
    afx_c_note_t *notes = NULL; uint32_t note_count = 0;
    if (read_file(song->midi, &midi, &midi_bytes) ||
        afx_c_midi_notes(midi, midi_bytes, song->tick_rate, &notes, &note_count)) goto failed;
    if (humanize_notes(notes, note_count, song->tick_rate, &song->humanize)) goto failed;
    if (song->release_tail_ms) {
        uint64_t tail = ((uint64_t)song->release_tail_ms * song->tick_rate + 999u) / 1000u;
        if (!tail || tail > UINT32_MAX) goto failed;
        for (uint32_t i = 0; i < note_count; ++i) {
            if (notes[i].end_tick > UINT32_MAX - tail) goto failed;
            notes[i].release_tick = notes[i].end_tick;
            notes[i].end_tick += (uint32_t)tail;
        }
    }
    for (uint32_t map_index = 0; map_index < map_count; ++map_index) {
        uint32_t matching = 0;
        for (uint32_t i = 0; i < note_count; ++i)
            if (find_map(maps, map_count, song->name, notes + i) == maps + map_index) ++matching;
        if (!matching) continue;
        afx_c_note_t *selected = malloc((size_t)matching * sizeof(*selected));
        if (!selected) goto failed;
        uint32_t at = 0;
        for (uint32_t i = 0; i < note_count; ++i)
            if (find_map(maps, map_count, song->name, notes + i) == maps + map_index) {
            selected[at] = notes[i]; selected[at].bank_msb = maps[map_index].sf2_bank / 128u;
            selected[at].bank_lsb = maps[map_index].sf2_bank % 128u; selected[at++].program = maps[map_index].sf2_program;
        }
        afx_c_sf2_output_t resolved = {0};
        maps[map_index].options.channel = maps[map_index].source->channel;
        int resolve_result = afx_c_sf2_resolve(maps[map_index].source->path, selected, matching,
                                               &maps[map_index].options, &resolved);
        int failed_resolve = resolve_result || append_resolved(&song->resolved, &resolved);
        free(selected); afx_c_sf2_output_free(&resolved);
        if (failed_resolve) {
            fprintf(stderr, "%s: cannot resolve MIDI program %u through SF2 preset %u\n", song->name,
                    maps[map_index].midi_program, maps[map_index].sf2_program);
            goto failed;
        }
    }
    for (uint32_t i = 0; i < note_count; ++i) if (!find_map(maps, map_count, song->name, notes + i)) goto failed;
    free(midi); free(notes); return song->resolved.note_count ? 0 : -1;
failed:
    free(midi); free(notes); return -1;
}

typedef struct { unsigned bank, program; } program_t;

static int collect_programs(const char *path, program_t **programs, uint32_t *count) {
    uint8_t *midi = NULL; uint32_t midi_bytes = 0;
    afx_c_note_t *notes = NULL; uint32_t note_count = 0;
    if (read_file(path, &midi, &midi_bytes) || afx_c_midi_notes(midi, midi_bytes, 1000, &notes, &note_count)) goto failed;
    for (uint32_t i = 0; i < note_count; ++i) {
        program_t item = {(unsigned)notes[i].bank_msb * 128u + notes[i].bank_lsb, notes[i].program};
        uint32_t at = 0;
        while (at < *count && ((*programs)[at].bank != item.bank || (*programs)[at].program != item.program)) ++at;
        if (at == *count) {
            program_t *grown = realloc(*programs, (size_t)(at + 1u) * sizeof(*grown));
            if (!grown) goto failed;
            *programs = grown; (*programs)[(*count)++] = item;
        }
    }
    free(midi); free(notes); return note_count ? 0 : -1;
failed:
    free(midi); free(notes); return -1;
}

static int create_map(int argc, char **argv) {
    /* argv: --create-map output.afbm soundfont.sf2 name midi [name midi ...] */
    if (argc < 6 || (argc - 4) & 1) return -1;
    program_t *programs = NULL; uint32_t program_count = 0;
    for (int i = 4; i < argc; i += 2)
        if (collect_programs(argv[i + 1], &programs, &program_count)) { free(programs); return -1; }
    char sf2_path[1200];
    if (map_relative_path(argv[2], argv[3], sf2_path)) { free(programs); return -1; }
    FILE *file = fopen(argv[2], "w");
    if (!file) { free(programs); return -1; }
    int failed = fprintf(file,
        "# AICAflow bank map. Edit source/map lines to combine SoundFonts or tune formats.\n"
        "# source <name> <soundfont.sf2> [stereo|left|right]\n"
        "# map <song|*> <midi-bank> <midi-program> <source> <sf2-bank> <sf2-program> <pcm16|pcm8|adpcm|auto> [key=value ...]\n"
        "# song <output-basename> <source.mid> [control-tick-rate] [release-tail-ms]\n\n"
        "source default \"%s\"\n\n", sf2_path) < 0;
    for (uint32_t i = 0; !failed && i < program_count; ++i)
        failed = fprintf(file, "map * %u %u default %u %u auto\n", programs[i].bank, programs[i].program,
                         programs[i].bank, programs[i].program) < 0;
    for (int i = 4; !failed && i < argc; i += 2) {
        char midi_path[1200];
        if (map_relative_path(argv[2], argv[i + 1], midi_path)) failed = 1;
        else failed = fprintf(file, "song %s \"%s\"\n", argv[i], midi_path) < 0;
    }
    failed |= fclose(file);
    free(programs); return failed ? -1 : 0;
}

static int build_per_song(const char *map_path, const char *directory) {
    source_t *sources = NULL; map_t *maps = NULL; song_t *songs = NULL;
    uint32_t source_count = 0, map_count = 0, song_count = 0;
    if (parse_map(map_path, &sources, &source_count, &maps, &map_count, &songs, &song_count)) goto failed;
    for (uint32_t song = 0; song < song_count; ++song) {
        afx_c_output_t out = {0};
        char afb[4096], afx[4096], afc[4096], afv[4096], afi[4096], named_afi[4096];
        if (resolve_song(songs + song, maps, map_count)) {
            fprintf(stderr, "%s: cannot resolve MIDI programs through its AFBM map\n", songs[song].name); goto failed;
        }
        if (afx_c_compile_zones(songs[song].resolved.notes, songs[song].resolved.note_count, songs[song].tick_rate,
                                songs[song].resolved.zones, songs[song].resolved.zone_count, &out)) {
            fprintf(stderr, "%s: cannot compile resolved sample zones\n", songs[song].name); goto failed;
        }
        if (out.afb_bytes < AFX_BANK_HEADER_BYTES || out.afb_bytes - AFX_BANK_HEADER_BYTES > AFX_TARGET_MAX_BANK_BYTES) {
            fprintf(stderr, "%s: AFB is %u bytes; AICA asset limit is %u bytes\n",
                    songs[song].name, out.afb_bytes - AFX_BANK_HEADER_BYTES, AFX_TARGET_MAX_BANK_BYTES); goto failed;
        }
        if (
            snprintf(afb, sizeof(afb), "%s/%s.afb", directory, songs[song].name) >= (int)sizeof(afb) ||
            flow_paths(directory, songs[song].name, afx, afc, afv) ||
            index_paths(afb, afi, named_afi) ||
            write_file(afb, out.afb, out.afb_bytes) || write_file(afx, out.afx, out.afx_bytes) ||
            write_file(afc, out.afc, out.afc_bytes) || write_file(afv, out.afv, out.afv_bytes) ||
            write_afi(afi, out.afb, out.afb_bytes, songs[song].resolved.zones,
                      songs[song].resolved.zone_count, songs[song].resolved.zone_names, 0) ||
            write_afi(named_afi, out.afb, out.afb_bytes, songs[song].resolved.zones,
                      songs[song].resolved.zone_count, songs[song].resolved.zone_names, 1)) {
            afx_c_output_free(&out); goto failed;
        }
        afx_c_output_free(&out);
    }
    free(sources); free(maps); free_songs(songs, song_count); return 0;
failed:
    free(sources); free(maps); free_songs(songs, song_count);
    fprintf(stderr, "cannot build per-song banks from %s\n", map_path); return 1;
}

/* Merge final AFB+AFX pairs into one bank.  This is deliberately an authoring
 * operation: runtime still sees its one-bank-per-flow ABI. */
typedef struct {
    const uint8_t *data;
    uint32_t bytes, offset;
    uint8_t format;
} merge_sample_t;
typedef struct {
    const char *path;
    uint8_t *afx, *afb, *afc;
    uint32_t afx_bytes, afb_bytes, afc_bytes;
    afx_file_header_t header;
    uint32_t *sample_for_setup;
} merge_flow_t;

static uint32_t merge_hash(const uint8_t *data, uint32_t bytes, uint32_t seed) {
    uint32_t hash = seed;
    for (uint32_t i = 0; i < bytes; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash ? hash : 1u;
}

static int sibling_path(const char *path, const char *suffix, char out[4096]) {
    const char *dot = strrchr(path, '.');
    size_t prefix = dot ? (size_t)(dot - path) : strlen(path);
    return prefix + strlen(suffix) >= 4096 ? -1 :
        (memcpy(out, path, prefix), strcpy(out + prefix, suffix), 0);
}

static int merged_path(const char *directory, const char *source, char out[4096]) {
    const char *base = strrchr(source, '/');
    base = base ? base + 1 : source;
    return snprintf(out, 4096, "%s/%s", directory, base) >= 4096 ? -1 : 0;
}

static int read_bank_bound_flow(merge_flow_t *flow) {
    char bank_path[4096], seek_path[4096];
    uint32_t data_at, payload_bytes, total;
    if (read_file(flow->path, &flow->afx, &flow->afx_bytes) ||
        afx_file_validate(flow->afx, flow->afx_bytes, &flow->header) ||
        sibling_path(flow->path, ".afb", bank_path) || sibling_path(flow->path, ".afc", seek_path) ||
        read_file(bank_path, &flow->afb, &flow->afb_bytes)) return -1;
    /* One-shot SFX do not seek.  Their AFX/AFB pair is still a normal merge
       input; do not force an otherwise useless AFC sidecar on that path. */
    FILE *seek = fopen(seek_path, "rb");
    if (seek) {
        fclose(seek);
        if (read_file(seek_path, &flow->afc, &flow->afc_bytes)) return -1;
    }
    data_at = afx_read32(flow->afb + 16); payload_bytes = afx_read32(flow->afb + 20); total = afx_read32(flow->afb + 24);
    if (flow->afb_bytes < AFX_BANK_HEADER_BYTES || afx_read32(flow->afb) != AFX_BANK_MAGIC ||
        afx_read32(flow->afb + 4) != AFX_BANK_VERSION || data_at != AFX_BANK_HEADER_BYTES ||
        total != flow->afb_bytes || payload_bytes != flow->afb_bytes - data_at ||
        afx_read32(flow->afb + 8) != flow->header.bank_id_low ||
        afx_read32(flow->afb + 12) != flow->header.bank_id_high ||
        (flow->afc && (flow->afc_bytes < AFX_SEEK_HEADER_BYTES || afx_read32(flow->afc) != AFX_SEEK_MAGIC ||
                       afx_read32(flow->afc + 4) != AFX_SEEK_VERSION || afx_read32(flow->afc + 8) != flow->header.control_id ||
                       afx_read32(flow->afc + 12) != flow->header.bank_id_low || afx_read32(flow->afc + 16) != flow->header.bank_id_high ||
                       afx_read32(flow->afc + 20) != AFX_SEEK_HEADER_BYTES ||
                       afx_read32(flow->afc + 24) != flow->afc_bytes - AFX_SEEK_HEADER_BYTES ||
                       afx_read32(flow->afc + 28) != flow->afc_bytes))) return -1;
    flow->sample_for_setup = calloc(flow->header.setup_count, sizeof(*flow->sample_for_setup));
    return flow->sample_for_setup ? 0 : -1;
}

static void free_merge_flows(merge_flow_t *flows, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        free(flows[i].afx); free(flows[i].afb); free(flows[i].afc); free(flows[i].sample_for_setup);
    }
    free(flows);
}

static int merge_sample_for(const uint8_t *data, uint32_t bytes, uint8_t format,
                            merge_sample_t **samples, uint32_t *count, uint32_t *capacity,
                            uint32_t *out) {
    for (uint32_t i = 0; i < *count; ++i)
        if ((*samples)[i].bytes == bytes && (*samples)[i].format == format &&
            !memcmp((*samples)[i].data, data, bytes)) {
            *out = i; return 0;
        }
    if (*count == *capacity) {
        uint32_t next = *capacity ? *capacity * 2u : 128u;
        merge_sample_t *grown = realloc(*samples, (size_t)next * sizeof(**samples));
        if (!grown) return -1;
        *samples = grown; *capacity = next;
    }
    /* These are final samples: merging must preserve the author's coding,
     * quality gate and loop representation, not choose a new encoding. */
    (*samples)[*count] = (merge_sample_t){data, bytes, 0, format};
    *out = (*count)++;
    return 0;
}

static int collect_merge_samples(merge_flow_t *flows, uint32_t flow_count,
                                 merge_sample_t **out_samples, uint32_t *out_count) {
    merge_sample_t *samples = NULL; uint32_t count = 0, capacity = 0;
    for (uint32_t flow_index = 0; flow_index < flow_count; ++flow_index) {
        merge_flow_t *flow = flows + flow_index;
        const uint8_t *image = flow->afx + flow->header.image_offset;
        uint32_t payload = afx_read32(flow->afb + 20);
        for (uint32_t setup = 0; setup < flow->header.setup_count; ++setup) {
            const uint8_t *relocation = flow->afx + flow->header.relocations_offset + setup * 12u;
            uint32_t pair = afx_read32(relocation), offset = afx_read32(relocation + 4), bytes = afx_read32(relocation + 8);
            uint16_t control = afx_read16(image + pair);
            uint32_t address = ((uint32_t)(control & 0x7fu) << 16) | afx_read16(image + pair + 2u);
            uint8_t format = (uint8_t)((control >> 7) & 3u);
            if (pair != setup * AFX_SETUP_BYTES || !bytes || format > AFX_ADPCM || offset != address ||
                offset > payload || bytes > payload - offset ||
                merge_sample_for(flow->afb + AFX_BANK_HEADER_BYTES + offset, bytes, format,
                                 &samples, &count, &capacity, flow->sample_for_setup + setup)) {
                free(samples); return -1;
            }
        }
    }
    *out_samples = samples; *out_count = count; return 0;
}

static int rewrite_seek(merge_flow_t *flow, const merge_sample_t *samples,
                        uint32_t bank_low, uint32_t bank_high, uint32_t control_id,
                        uint8_t **out, uint32_t *out_bytes) {
    uint8_t *data = malloc(flow->afc_bytes);
    uint32_t cursor, entries;
    if (!data) return -1;
    memcpy(data, flow->afc, flow->afc_bytes);
    if (flow->afc_bytes < AFX_SEEK_HEADER_BYTES + 16u || afx_read32(data + AFX_SEEK_HEADER_BYTES) != 0x31504b43u ||
        afx_read32(data + AFX_SEEK_HEADER_BYTES + 4) != 1u || afx_read32(data + AFX_SEEK_HEADER_BYTES + 12) != 0u) goto failed;
    entries = afx_read32(data + AFX_SEEK_HEADER_BYTES + 8); cursor = AFX_SEEK_HEADER_BYTES + 16u;
    for (uint32_t entry = 0; entry < entries; ++entry) {
        if (cursor > flow->afc_bytes || flow->afc_bytes - cursor < 16u) goto failed;
        uint32_t states = afx_read32(data + cursor + 12); cursor += 16u;
        for (uint32_t state = 0; state < states; ++state) {
            if (cursor > flow->afc_bytes || flow->afc_bytes - cursor < 40u) goto failed;
            uint16_t control = afx_read16(data + cursor + 4), low = afx_read16(data + cursor + 6);
            if (!(control & 0x400u)) {
                uint32_t old = ((uint32_t)(control & 0x7fu) << 16) | low, setup = 0;
                while (setup < flow->header.setup_count) {
                    const uint8_t *r = flow->afx + flow->header.relocations_offset + setup * 12u;
                    uint32_t offset = afx_read32(r + 4), bytes = afx_read32(r + 8);
                    if (old >= offset && old - offset < bytes) break;
                    ++setup;
                }
                if (setup == flow->header.setup_count) goto failed;
                const merge_sample_t *sample = samples + flow->sample_for_setup[setup];
                uint32_t address = sample->offset + old - afx_read32(flow->afx + flow->header.relocations_offset + setup * 12u + 4u);
                if (address > 0x7fffffu) goto failed;
                afx_write16(data + cursor + 4, (uint16_t)((control & ~0x1ffu) | ((uint16_t)sample->format << 7) | (address >> 16)));
                afx_write16(data + cursor + 6, (uint16_t)address);
            }
            cursor += 40u;
        }
    }
    if (cursor != flow->afc_bytes) goto failed;
    afx_write32(data + 8, control_id); afx_write32(data + 12, bank_low); afx_write32(data + 16, bank_high);
    *out = data; *out_bytes = flow->afc_bytes; return 0;
failed:
    free(data); return -1;
}

static int merge_final_banks(const char *bank_path, const char *controls_dir, int argc, char **argv) {
    merge_flow_t *flows = calloc((size_t)argc, sizeof(*flows));
    merge_sample_t *samples = NULL; uint8_t *bank = NULL; uint32_t flow_count = (uint32_t)argc, sample_count = 0, payload = 0;
    int result = 1;
    if (!flows) goto done;
    for (uint32_t i = 0; i < flow_count; ++i) {
        flows[i].path = argv[i];
        if (read_bank_bound_flow(flows + i)) goto done;
    }
    if (collect_merge_samples(flows, flow_count, &samples, &sample_count)) goto done;
    for (uint32_t i = 0; i < sample_count; ++i) {
        payload = align32(payload);
        if (samples[i].bytes > AFX_TARGET_MAX_BANK_BYTES - payload) goto done;
        samples[i].offset = payload; payload += samples[i].bytes;
    }
    if (!payload || payload > AFX_TARGET_MAX_BANK_BYTES || payload > UINT32_MAX - AFX_BANK_HEADER_BYTES) goto done;
    bank = calloc(1, AFX_BANK_HEADER_BYTES + payload);
    if (!bank) goto done;
    for (uint32_t i = 0; i < sample_count; ++i) memcpy(bank + AFX_BANK_HEADER_BYTES + samples[i].offset, samples[i].data, samples[i].bytes);
    uint32_t bank_low = merge_hash(bank + AFX_BANK_HEADER_BYTES, payload, 2166136261u);
    uint32_t bank_high = merge_hash(bank + AFX_BANK_HEADER_BYTES, payload, 2166136261u ^ 0x9e3779b9u);
    afx_write32(bank, AFX_BANK_MAGIC); afx_write32(bank + 4, AFX_BANK_VERSION); afx_write32(bank + 8, bank_low); afx_write32(bank + 12, bank_high);
    afx_write32(bank + 16, AFX_BANK_HEADER_BYTES); afx_write32(bank + 20, payload); afx_write32(bank + 24, AFX_BANK_HEADER_BYTES + payload);
    if (write_file(bank_path, bank, AFX_BANK_HEADER_BYTES + payload)) goto done;
    for (uint32_t i = 0; i < flow_count; ++i) {
        merge_flow_t *flow = flows + i; uint8_t *rewritten = malloc(flow->afx_bytes), *seek = NULL; uint32_t seek_bytes = 0;
        char afx_path[4096], afc_path[4096];
        if (!rewritten || merged_path(controls_dir, flow->path, afx_path) || sibling_path(afx_path, ".afc", afc_path)) { free(rewritten); goto done; }
        memcpy(rewritten, flow->afx, flow->afx_bytes);
        for (uint32_t setup = 0; setup < flow->header.setup_count; ++setup) {
            uint8_t *state = rewritten + flow->header.image_offset + setup * AFX_SETUP_BYTES;
            uint8_t *relocation = rewritten + flow->header.relocations_offset + setup * 12u;
            const merge_sample_t *sample = samples + flow->sample_for_setup[setup]; uint16_t control = afx_read16(state);
            if (sample->offset > 0x7fffffu) { free(rewritten); goto done; }
            afx_write16(state, (uint16_t)((control & ~0x1ffu) | ((uint16_t)sample->format << 7) | (sample->offset >> 16)));
            afx_write16(state + 2, (uint16_t)sample->offset); afx_write32(relocation + 4, sample->offset); afx_write32(relocation + 8, sample->bytes);
        }
        afx_write32(rewritten + 40, bank_low); afx_write32(rewritten + 44, bank_high);
        uint32_t control_id = afx_control_id(rewritten + flow->header.image_offset, flow->header.image_size);
        afx_write32(rewritten + 32, control_id);
        if (afx_file_validate(rewritten, flow->afx_bytes, NULL) ||
            (flow->afc && rewrite_seek(flow, samples, bank_low, bank_high, control_id, &seek, &seek_bytes)) ||
            write_file(afx_path, rewritten, flow->afx_bytes) ||
            (flow->afc && write_file(afc_path, seek, seek_bytes))) {
            free(rewritten); free(seek); goto done;
        }
        free(rewritten); free(seek);
    }
    printf("merged %u flows and %u samples into %s (%u bytes)\n", flow_count, sample_count, bank_path, payload);
    result = 0;
done:
    if (result) fprintf(stderr, "cannot merge final AFB/AFX inputs\n");
    free(bank); free(samples); free_merge_flows(flows, flow_count); return result;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--create-map")) {
        if (create_map(argc, argv))
            return fprintf(stderr, "usage: %s --create-map library.afbm bank.sf2 name song.mid [name song.mid ...]\n", argv[0]), 2;
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--per-song")) return build_per_song(argv[2], argv[3]);
    if (argc >= 5 && !strcmp(argv[1], "--merge")) return merge_final_banks(argv[2], argv[3], argc - 4, argv + 4);
    if (argc != 4) return fprintf(stderr,
        "usage: %s library.afbm output-dir bank.afb\n"
        "       %s --per-song library.afbm output-dir\n"
        "       %s --merge bank.afb controls-dir flow.afx [flow.afx ...]\n", argv[0], argv[0], argv[0]), 2;
    source_t *sources = NULL; map_t *maps = NULL; song_t *songs = NULL;
    uint32_t source_count = 0, map_count = 0, song_count = 0, zone_count = 0;
    afx_c_zone_t *zones = NULL;
    char (*zone_names)[AFX_C_SAMPLE_NAME_BYTES] = NULL;
    if (parse_map(argv[1], &sources, &source_count, &maps, &map_count, &songs, &song_count)) goto failed;
    for (uint32_t song = 0; song < song_count; ++song) {
        if (resolve_song(songs + song, maps, map_count) || songs[song].resolved.zone_count > UINT32_MAX - zone_count ||
            songs[song].resolved.note_count > UINT16_MAX) goto failed;
        afx_c_zone_t *grown = realloc(zones, (size_t)(zone_count + songs[song].resolved.zone_count) * sizeof(*zones));
        if (!grown) goto failed;
        zones = grown;
        char (*grown_names)[AFX_C_SAMPLE_NAME_BYTES] = realloc(zone_names,
            (size_t)(zone_count + songs[song].resolved.zone_count) * sizeof(*grown_names));
        if (!grown_names) goto failed;
        zone_names = grown_names;
        memcpy(zones + zone_count, songs[song].resolved.zones,
               (size_t)songs[song].resolved.zone_count * sizeof(*zones));
        memcpy(zone_names + zone_count, songs[song].resolved.zone_names,
               (size_t)songs[song].resolved.zone_count * sizeof(*zone_names));
        for (uint32_t note = 0; note < songs[song].resolved.note_count; ++note) {
            uint32_t setup = zone_count + songs[song].resolved.notes[note].setup_index;
            if (!setup || setup > UINT16_MAX) goto failed;
            songs[song].resolved.notes[note].setup_index = (uint16_t)setup;
        }
        zone_count += songs[song].resolved.zone_count;
    }
    if (!zone_count) goto failed;
    uint32_t bank_low = 0, bank_high = 0;
    for (uint32_t song = 0; song < song_count; ++song) {
        afx_c_output_t out;
        if (afx_c_compile_zones(songs[song].resolved.notes, songs[song].resolved.note_count, songs[song].tick_rate,
                                zones, zone_count, &out)) goto failed;
        if (out.afb_bytes < AFX_BANK_HEADER_BYTES || out.afb_bytes - AFX_BANK_HEADER_BYTES > AFX_TARGET_MAX_BANK_BYTES) {
            afx_c_output_free(&out); goto failed;
        }
        if (!song) {
            bank_low = afx_read32(out.afb + 8); bank_high = afx_read32(out.afb + 12);
            char afi[4096], named_afi[4096];
            if (write_file(argv[3], out.afb, out.afb_bytes) || index_paths(argv[3], afi, named_afi) ||
                write_afi(afi, out.afb, out.afb_bytes, zones, zone_count, zone_names, 0) ||
                write_afi(named_afi, out.afb, out.afb_bytes, zones, zone_count, zone_names, 1)) {
                afx_c_output_free(&out); goto failed;
            }
        }
        if (afx_read32(out.afx + 40) != bank_low || afx_read32(out.afx + 44) != bank_high) {
            afx_c_output_free(&out); goto failed;
        }
        char afx[4096], afc[4096], afv[4096];
        int error = flow_paths(argv[2], songs[song].name, afx, afc, afv) ||
                    write_file(afx, out.afx, out.afx_bytes) || write_file(afc, out.afc, out.afc_bytes) ||
                    write_file(afv, out.afv, out.afv_bytes);
        afx_c_output_free(&out);
        if (error) goto failed;
    }
    free(sources); free(maps); free(zone_names); free(zones); free_songs(songs, song_count); return 0;
failed:
    free(sources); free(maps); free(zone_names); free(zones); free_songs(songs, song_count);
    fprintf(stderr, "cannot build shared bank from %s\n", argv[1]); return 1;
}
