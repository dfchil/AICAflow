/* Hardware check of firmware ABI 13 (make -C driver banks-check), with the
 * looping quickstart fixture of loop-check: a flow whose setup plays from bank
 * slot 1 sounds from that bank, read back from the AICA's sample address
 * registers beside an unchanged one-bank instance, and keeps both banks until
 * it is freed. */
#include <kos.h>
#include <aicaflow/bank.h>
#include <aicaflow/host.h>
#include <aicaflow/codec.h>
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

static int failures;
static void check(const char *name, int ok) {
    printf("AFX_BANKS %s %s\n", ok ? "ok  " : "FAIL", name);
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

/* The fixture's bank again under another identity: the scene bank. */
alignas(32) static unsigned char scene_file[sizeof(bank_file)];
#define SCENE_ID_LOW 0x5ce7e001u
#define SCENE_ID_HIGH 0x5ce7e002u

/* The fixture's flow with every setup in bank slot 1: the relocations, a bank
 * list naming the scene bank, then the image with the bank table after its
 * stream. Returns the file's size, or zero. */
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
    afx_write32(banked_file + list + 4, SCENE_ID_LOW);
    afx_write32(banked_file + list + 8, SCENE_ID_HIGH);
    return size;
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
static int running(afx_instance_t instance) {
    afx_instance_status_t status = {0};
    return !afx_instance_status(instance, &status) && status.state == AFX_RUNNING &&
           status.result == AFX_OK;
}

int main(void) {
    afx_file_header_t header = {0};
    (void)afx_file_validate_layout(flow_file, sizeof(flow_file), &header);
    uint32_t note_tick = 0, address = 0, next_tick = 0;
    uint32_t banked_size = banked(&header);
    int result = afx_init(firmware, sizeof(firmware));
    printf("AFX_BANKS init=%d banked=%lu\n", result, (unsigned long)banked_size);
    if (result || !banked_size || !channel0_note(&header, &note_tick, &address, &next_tick)) return 1;
    afx_set_end_callback(on_end, NULL);
    afx_mem_stats_t empty, after;
    (void)afx_mem_stats(&empty);

    memcpy(scene_file, bank_file, sizeof(scene_file));
    afx_write32(scene_file + 8, SCENE_ID_LOW);
    afx_write32(scene_file + 12, SCENE_ID_HIGH);
    afx_bank_t core = {0}, scene = {0};
    afx_asset_t plain = 0, flow = 0;
    const afx_bank_t *banks[] = {&core, &scene};
    if ((result = afx_bank_load_memory(&core, bank_file, sizeof(bank_file))) ||
        (result = afx_bank_load_memory(&scene, scene_file, sizeof(scene_file))) ||
        (result = afx_bank_flow_upload(&core, flow_file, sizeof(flow_file), &plain)) ||
        (result = afx_flow_upload(banks, 2, banked_file, banked_size, &flow))) {
        printf("AFX_BANKS load=%d\n", result);
        afx_shutdown();
        return 1;
    }

    /* The banked instance on channel 0, the plain one on the next channels. */
    afx_instance_t probe, control;
    int a = afx_instance_activate(flow, &probe);
    int c = afx_instance_activate(plain, &control);
    uint32_t start = afx_instance_start_tick(probe);
    while ((int32_t)(afx_status_timer_ticks() - start) < (int32_t)note_tick + 2) {}
    uint32_t probe_address = sample_register(0);
    uint32_t control_address = sample_register(header.required_channels);
    int32_t read_at = (int32_t)(afx_status_timer_ticks() - start);
    uint32_t core_base = afx_asset_addr(core.asset), scene_base = afx_asset_addr(scene.asset);
    printf("AFX_BANKS relative=%05lx core=%06lx scene=%06lx probe=%06lx control=%06lx read=%ld next=%lu\n",
           (unsigned long)address, (unsigned long)core_base, (unsigned long)scene_base,
           (unsigned long)probe_address, (unsigned long)control_address, (long)read_at,
           (unsigned long)next_tick);
    thd_sleep(50);
    int both_running = running(probe) && running(control);
    if (control_address != core_base + address) {
        printf("AFX_BANKS inconclusive: the one-bank instance did not read back its sample address\n");
        ++failures;
    } else {
        check("slot 1 plays from the scene bank", !a && !c && (uint32_t)read_at < next_tick &&
              probe_address == scene_base + address);
    }
    check("both instances run without an error", both_running);

    /* Released banks outlive the release until the flows that play from them are freed. */
    (void)afx_instance_stop(probe);
    (void)afx_instance_stop(control);
    int ended = wait_ended(2, 1000);
    int released = !afx_asset_release(core.asset) && !afx_asset_release(scene.asset);
    int kept = afx_asset_addr(core.asset) && afx_asset_addr(scene.asset);
    int freed = !afx_asset_free(flow) && !afx_asset_addr(scene.asset) && afx_asset_addr(core.asset) &&
                !afx_asset_free(plain) && !afx_asset_addr(core.asset);
    (void)afx_mem_stats(&after);
    check("banks freed with the last flow", ended && released && kept && freed &&
          after.free_bytes == empty.free_bytes && after.largest_free_block == empty.largest_free_block);
    printf("AFX_BANKS free=%lu/%lu largest=%lu/%lu\n", (unsigned long)after.free_bytes,
           (unsigned long)empty.free_bytes, (unsigned long)after.largest_free_block,
           (unsigned long)empty.largest_free_block);
    afx_shutdown();
    printf("AFX_BANKS %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
