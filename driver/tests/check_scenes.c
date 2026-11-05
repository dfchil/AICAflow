/* Hardware check of AFS scenes (make -C driver scenes-check), with the looping
 * quickstart fixture of loop-check. A permanent scene (the core bank, a flow
 * and the DSP ring) stays loaded while a level scene and its song slot are
 * loaded, played and released again and again. The level's flow plays from
 * the core bank and from its own, read back from the AICA's sample address
 * registers, and the arena's largest free block must come back every time. */
#include <kos.h>
#include <aicaflow/bank.h>
#include <aicaflow/host.h>
#include <aicaflow/codec.h>
#include <aicaflow/scene.h>
#include <stdio.h>
#include <stdalign.h>

alignas(32) static const unsigned char firmware[] = {
#embed "../../firmware/aicaflow.drv"
};
alignas(32) static const unsigned char flow_file[] = {
#embed "../build/loop.afx"
};
alignas(32) static const unsigned char bank_file[] = {
#embed "../build/loop.afb"
};

#define LEVELS 8u
#define SONGS 3u

static int failures;
static void check(const char *name, int ok) {
    printf("AFX_SCENES %s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) ++failures;
}
static volatile unsigned ended_count;
static void on_end(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    (void)instance; (void)status; (void)user;
    ++ended_count;
}
static int wait_ended(unsigned count, unsigned ms) {
    uint64_t start = timer_ms_gettime64();
    while (ended_count < count && timer_ms_gettime64() - start < ms) thd_sleep(1);
    return ended_count >= count;
}
static uint32_t align32(uint32_t value) { return (value + 31u) & ~31u; }

/* The fixture's bank under another identity, and its flow bound to that bank. */
#define SCENE_ID 0x5ce7e001u
#define SONG_ID 0x5096e001u
alignas(32) static unsigned char scene_bank[sizeof(bank_file)], song_bank[sizeof(bank_file)];
alignas(32) static unsigned char song_flow[sizeof(flow_file)];

/* The fixture's flow with every setup in bank slot 1, the scene bank's: the
 * relocations, a bank list naming it, then the image with the bank table after
 * its stream. Returns the file's size, or zero. */
alignas(32) static unsigned char banked_file[sizeof(flow_file) + 256];
static uint32_t banked(const afx_file_header_t *h) {
    uint32_t relocations = AFX_FILE_HEADER_BYTES, list = relocations + h->relocation_count * 12u;
    uint32_t image = (list + 12u + 31u) & ~31u;
    uint32_t table = AFX_BANK_TABLE_OFFSET(h->stream_offset + h->stream_size);
    uint32_t size = image + table + AFX_BANK_TABLE_BYTES;
    if (size > sizeof(banked_file) || h->image_size > table) return 0;
    memset(banked_file, 0, sizeof(banked_file));
    afx_file_header_t out = *h;
    out.flags |= AFX_FLAG_BANKS;
    out.total_size = size;
    out.image_offset = image;
    out.image_size = table + AFX_BANK_TABLE_BYTES;
    out.relocations_offset = relocations;
    out.banks_offset = list;
    afx_encode_header(banked_file, &out);
    memcpy(banked_file + image, flow_file + h->image_offset, h->image_size);
    for (uint32_t i = 0; i < h->relocation_count; ++i) {
        const unsigned char *from = flow_file + h->relocations_offset + i * 12u;
        unsigned char *to = banked_file + relocations + i * 12u;
        afx_write32(to, afx_read32(from));
        afx_write32(to + 4, afx_read32(from + 4) | 1u << AFX_BANK_SLOT_SHIFT);
        afx_write32(to + 8, afx_read32(from + 8));
        unsigned char *control = banked_file + image + afx_read32(from);
        afx_write16(control, afx_read16(control) | 1u << (AFX_BANK_SLOT_SHIFT - 16u));
    }
    afx_write32(banked_file + list, 1);
    afx_write32(banked_file + list + 4, SCENE_ID);
    afx_write32(banked_file + list + 8, SCENE_ID);
    return size;
}

/* An AFS file of entries, in order: header, entries, names, files. */
typedef struct {
    uint32_t kind;
    const char *name;
    const unsigned char *file;
    uint32_t file_size, offset, bytes;
} part_t;
alignas(32) static unsigned char permanent[sizeof(bank_file) + sizeof(flow_file) + 512];
alignas(32) static unsigned char level[sizeof(bank_file) + sizeof(banked_file) + 512];
static uint32_t scene(unsigned char *out, uint32_t capacity, uint32_t id, uint32_t block_bytes,
                      const part_t *parts, uint32_t count) {
    uint32_t at = AFX_SCENE_HEADER_BYTES + count * AFX_SCENE_ENTRY_BYTES;
    memset(out, 0, capacity);
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char *entry = out + AFX_SCENE_HEADER_BYTES + i * AFX_SCENE_ENTRY_BYTES;
        afx_write32(entry, parts[i].kind);
        afx_write32(entry + 4, at);
        memcpy(out + at, parts[i].name, strlen(parts[i].name) + 1);
        at += strlen(parts[i].name) + 1;
        afx_write32(entry + 16, parts[i].offset);
        afx_write32(entry + 20, parts[i].bytes);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!parts[i].file) continue;
        unsigned char *entry = out + AFX_SCENE_HEADER_BYTES + i * AFX_SCENE_ENTRY_BYTES;
        at = align32(at);
        if (at + parts[i].file_size > capacity) return 0;
        memcpy(out + at, parts[i].file, parts[i].file_size);
        afx_write32(entry + 8, at);
        afx_write32(entry + 12, parts[i].file_size);
        at += parts[i].file_size;
    }
    afx_write32(out, AFX_SCENE_MAGIC); afx_write32(out + 4, AFX_SCENE_VERSION);
    afx_write32(out + 8, id); afx_write32(out + 16, at);
    afx_write32(out + 20, block_bytes); afx_write32(out + 24, count);
    return at;
}

/* Local channel 0's first NOTE: its authored tick and bank-relative sample
 * address, and the tick of the channel's next NOTE (UINT32_MAX for none). */
static int channel0_note(const afx_file_header_t *header, uint32_t *tick_out,
                         uint32_t *address_out, uint32_t *next_out) {
    const uint8_t *image = flow_file + header->image_offset;
    const uint8_t *stream = image + header->stream_offset;
    uint32_t at = 0, tick = 0, found = 0;
    *next_out = UINT32_MAX;
    while (at < header->stream_size) {
        afx_event_t event;
        if (afx_decode_event(stream + at, header->stream_size - at, &event)) return 0;
        if ((event.opcode == AFX_OP_NOTE) && event.channel == 0) {
            if (found++) { *next_out = tick; break; }
            uint16_t words[AFX_FIELD_COUNT];
            if (afx_apply_setup_fields(words, image + event.setup * AFX_SETUP_BYTES, event.mask,
                                       event.values, afx_field_value_bytes(event.mask))) return 0;
            *tick_out = tick;
            *address_out = ((uint32_t)(words[AFX_FIELD_CONTROL] & 0x7fu) << 16) |
                           words[AFX_FIELD_SAMPLE_LOW];
        }
        if (event.opcode == AFX_OP_WAIT8 || event.opcode == AFX_OP_WAIT16 || event.opcode == AFX_OP_WAIT32)
            tick += event.wait;
        if (event.opcode == AFX_OP_LOOP || event.opcode == AFX_OP_END) break;
        at += event.bytes;
    }
    return found != 0;
}
/* The sample address a channel's registers hold. */
static uint32_t sample_register(uint32_t physical) {
    uint32_t base = 0xa0700000u + physical * AFX_AICA_CHANNEL_REG_STRIDE;
    return ((g2_read_32(base + AFX_FIELD_CONTROL * 4u) & 0x7fu) << 16) |
           (g2_read_32(base + AFX_FIELD_SAMPLE_LOW * 4u) & 0xffffu);
}
static uint32_t entry_addr(afx_scene_t s, uint32_t index) {
    afx_scene_info_t info;
    return afx_scene_entry(s, index, &info) ? 0 : info.addr;
}

int main(void) {
    afx_file_header_t header = {0};
    (void)afx_file_validate_layout(flow_file, sizeof(flow_file), &header);
    uint32_t note_tick = 0, address = 0, next_tick = 0;
    uint32_t banked_size = banked(&header);
    uint32_t payload = afx_read32(bank_file + 20), payload_at = afx_read32(bank_file + 16);
    uint32_t bank_room = align32(payload), flow_room = align32(header.image_size);
    uint32_t banked_room = align32(afx_read32(banked_file + 20)); /* Its image, with the bank table. */

    memcpy(scene_bank, bank_file, sizeof(bank_file));
    afx_write32(scene_bank + 8, SCENE_ID); afx_write32(scene_bank + 12, SCENE_ID);
    memcpy(song_bank, bank_file, sizeof(bank_file));
    afx_write32(song_bank + 8, SONG_ID); afx_write32(song_bank + 12, SONG_ID);
    memcpy(song_flow, flow_file, sizeof(flow_file));
    afx_write32(song_flow + 40, SONG_ID); afx_write32(song_flow + 44, SONG_ID);
    uint32_t ring_at = (bank_room + flow_room + 2047u) & ~2047u;
    const part_t permanent_parts[] = {
        {AFX_SCENE_BANK, "core", bank_file, sizeof(bank_file), 0, bank_room},
        {AFX_SCENE_FLOW, "loop", flow_file, sizeof(flow_file), bank_room, flow_room},
        {AFX_SCENE_RING, "ring", NULL, 0, ring_at, AFX_SCENE_RING_MIN_BYTES},
    };
    const part_t level_parts[] = {
        {AFX_SCENE_BANK, "scene", scene_bank, sizeof(scene_bank), 0, bank_room},
        {AFX_SCENE_FLOW, "banked", banked_file, banked_size, bank_room, banked_room},
        {AFX_SCENE_SLOT, "song", NULL, 0, bank_room + banked_room, bank_room + flow_room},
    };
    uint32_t permanent_size = scene(permanent, sizeof(permanent), 0x9e7a, ring_at + AFX_SCENE_RING_MIN_BYTES,
                                    permanent_parts, 3);
    uint32_t level_size = scene(level, sizeof(level), 0x1e7e1, bank_room * 2u + banked_room + flow_room,
                                level_parts, 3);
    int result = afx_init(firmware, sizeof(firmware));
    printf("AFX_SCENES init=%d banked=%lu permanent=%lu level=%lu payload=%lu@%lu\n", result,
           (unsigned long)banked_size, (unsigned long)permanent_size, (unsigned long)level_size,
           (unsigned long)payload, (unsigned long)payload_at);
    if (result || !banked_size || !permanent_size || !level_size ||
        !channel0_note(&header, &note_tick, &address, &next_tick)) return 1;
    afx_set_end_callback(on_end, NULL);
    afx_mem_stats_t empty, base, now;
    (void)afx_mem_stats(&empty);

    afx_scene_t p = 0;
    if ((result = afx_scene_load(permanent, permanent_size, NULL, 0, &p))) {
        printf("AFX_SCENES permanent=%d\n", result);
        afx_shutdown();
        return 1;
    }
    uint32_t core_base = entry_addr(p, 0);
    (void)afx_mem_stats(&base);
    afx_dsp_program_t dsp;
    int dsp_ok = !afx_dsp_program_delay(&dsp, 100, 8192, false) &&
                 !afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0);
    (void)afx_mem_stats(&now);
    check("the DSP takes the permanent scene's ring", dsp_ok && now.used_bytes == base.used_bytes &&
          base.used_bytes - empty.used_bytes == ring_at + AFX_SCENE_RING_MIN_BYTES);

    /* Each level: load it, fill the slot, play both, reload the slot, release. */
    unsigned played = 0, kept = 0, reloads = 0, restored = 0;
    uint32_t last_probe = 0, last_control = 0, last_scene = 0, last_song = 0;
    for (unsigned round = 0; round < LEVELS; ++round) {
        afx_scene_t l = 0;
        afx_asset_t flow = 0, song = 0;
        if ((result = afx_scene_load(level, level_size, &p, 1, &l)) ||
            (result = afx_scene_find(l, "banked", &flow))) {
            printf("AFX_SCENES level %u=%d\n", round, result);
            break;
        }
        uint32_t scene_base = entry_addr(l, 0);
        for (unsigned take = 0; take < SONGS; ++take) {
            if ((result = afx_scene_slot_load(l, "song", song_bank, sizeof(song_bank), song_flow,
                                              sizeof(song_flow), &song))) {
                printf("AFX_SCENES slot %u.%u=%d\n", round, take, result);
                break;
            }
            ++reloads;
            uint32_t song_base = entry_addr(l, 2);
            afx_instance_t probe = 0, control = 0;
            unsigned ended = ended_count;
            int a = afx_instance_activate(flow, &probe);
            int c = afx_instance_activate(song, &control);
            uint32_t start = afx_instance_start_tick(probe);
            while ((int32_t)(afx_status_timer_ticks() - start) < (int32_t)note_tick + 2) {}
            uint32_t probe_address = sample_register(0);
            uint32_t control_address = sample_register(header.required_channels);
            int32_t read_at = (int32_t)(afx_status_timer_ticks() - start);
            last_probe = probe_address; last_control = control_address;
            last_scene = scene_base; last_song = song_base;
            played += !a && !c && (uint32_t)read_at < next_tick &&
                      probe_address == scene_base + address && control_address == song_base + address;
            /* Reloading a slot whose flow plays is refused, and keeps it. */
            afx_asset_t refused = 0, found = 0;
            kept += afx_scene_slot_load(l, "song", song_bank, sizeof(song_bank), song_flow,
                                        sizeof(song_flow), &refused) == -AFX_ASSET_REFERENCED &&
                    !afx_scene_find(l, "song", &found) && found == song;
            (void)afx_instance_stop(probe);
            (void)afx_instance_stop(control);
            if (!wait_ended(ended + 2, 1000)) printf("AFX_SCENES round %u: no end\n", round);
        }
        (void)afx_scene_release(l);
        (void)afx_mem_stats(&now);
        restored += now.free_bytes == base.free_bytes && now.largest_free_block == base.largest_free_block;
    }
    printf("AFX_SCENES core=%06lx scene=%06lx song=%06lx probe=%06lx control=%06lx relative=%05lx\n",
           (unsigned long)core_base, (unsigned long)last_scene, (unsigned long)last_song,
           (unsigned long)last_probe, (unsigned long)last_control, (unsigned long)address);
    check("level flows play from the level bank, songs from the slot", played == LEVELS * SONGS);
    check("a slot whose flow plays keeps it", kept == LEVELS * SONGS && reloads == LEVELS * SONGS);
    check("every released level gives its whole block back", restored == LEVELS);

    /* The permanent scene keeps its ring while the DSP uses it. */
    int held = afx_scene_release(p) == -AFX_ASSET_REFERENCED;
    int released = !afx_dsp_scene_disable() && !afx_scene_release(p);
    (void)afx_mem_stats(&now);
    check("the permanent scene goes once the DSP is off", held && released &&
          now.free_bytes == empty.free_bytes && now.largest_free_block == empty.largest_free_block);
    printf("AFX_SCENES free=%lu/%lu largest=%lu/%lu\n", (unsigned long)now.free_bytes,
           (unsigned long)empty.free_bytes, (unsigned long)now.largest_free_block,
           (unsigned long)empty.largest_free_block);
    afx_shutdown();
    printf("AFX_SCENES %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
