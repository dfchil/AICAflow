/* Hardware check of the command semantics of firmware ABI 12 (make -C driver
 * commands-check), with the long quickstart fixture of tests/make_fixtures.py:
 * STOP right behind ACTIVATE, a channel reused in the frame it is stopped,
 * afx_instance_replace with every channel in use, afx_asset_release, and a
 * PATCH sent right after ACTIVATE that the channel's first NOTE keeps. */
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
#embed "../build/long.afx"
};
alignas(32) static const unsigned char bank_file[] = {
#embed "../build/long.afb"
};

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
static int failures;
static void check(const char *name, int ok) {
    printf("AFX_CMD %s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) ++failures;
}
static int load(afx_bank_t *bank, afx_asset_t *flow) {
    *bank = (afx_bank_t){0};
    int result = afx_bank_load_memory(bank, bank_file, sizeof(bank_file));
    return result ? result : afx_bank_flow_upload(bank, flow_file, sizeof(flow_file), flow);
}
/* Local channel 0's first NOTE: its authored tick and DIRECT word, and the
 * tick of the channel's next NOTE (UINT32_MAX for none in the loop body). */
static int channel0_note(const afx_file_header_t *header, uint32_t *tick_out,
                         uint16_t *direct_out, uint32_t *next_out) {
    const uint8_t *image = flow_file + header->image_offset;
    const uint8_t *stream = image + header->stream_offset;
    uint32_t at = 0, tick = 0, found = 0;
    *next_out = UINT32_MAX;
    while (at < header->stream_size) {
        afx_event_t event;
        if (afx_decode_event(stream + at, header->stream_size - at, &event)) return 0;
        if (event.opcode == AFX_OP_NOTE && event.channel == 0) {
            if (found++) { *next_out = tick; break; }
            uint16_t words[AFX_FIELD_COUNT];
            if (afx_apply_setup_fields(words, image + event.setup * AFX_SETUP_BYTES, event.mask,
                                       event.values, afx_field_value_bytes(event.mask))) return 0;
            *tick_out = tick;
            *direct_out = words[AFX_FIELD_DIRECT];
        }
        if (event.opcode == AFX_OP_WAIT8 || event.opcode == AFX_OP_WAIT16 || event.opcode == AFX_OP_WAIT32)
            tick += event.wait;
        if (event.opcode == AFX_OP_END) break;
        at += event.bytes;
    }
    return found != 0;
}
/* Activations faster than the ARM7 takes them fill the command ring. */
static int activate(afx_asset_t flow, afx_instance_t *out) {
    uint64_t start = timer_ms_gettime64();
    int result;
    while ((result = afx_instance_activate(flow, out)) == -AFX_IPC_FULL &&
           timer_ms_gettime64() - start < 1000) thd_pass();
    return result;
}
static int running(afx_instance_t instance) {
    uint64_t start = timer_ms_gettime64();
    afx_instance_status_t status = {0};
    while (timer_ms_gettime64() - start < 1000 && !afx_instance_status(instance, &status) &&
           status.state != AFX_RUNNING) thd_sleep(1);
    return status.state == AFX_RUNNING;
}
/* The loop fixture asking for 16 channels, of which it plays one: four fill the AICA. */
alignas(32) static unsigned char wide_file[sizeof(flow_file)];
static uint16_t direct_register(uint32_t physical) {
    return (uint16_t)g2_read_32(0xa0700000u + physical * AFX_AICA_CHANNEL_REG_STRIDE + AFX_FIELD_DIRECT * 4u);
}

int main(void) {
    afx_file_header_t header = {0};
    (void)afx_file_validate_layout(flow_file, sizeof(flow_file), &header);
    int result = afx_init(firmware, sizeof(firmware));
    printf("AFX_CMD init=%d channels=%lu\n", result, (unsigned long)header.required_channels);
    if (result) return 1;
    afx_set_end_callback(on_end, NULL);
    afx_bank_t bank;
    afx_asset_t flow;
    if ((result = load(&bank, &flow))) {
        printf("AFX_CMD load=%d\n", result);
        afx_shutdown();
        return 1;
    }
    unsigned expected_ends = 0;

    /* STOP right behind ACTIVATE: no AFX_BUSY, and the instance ends. */
    afx_instance_t first;
    int activated = afx_instance_activate(flow, &first);
    int stopped = afx_instance_stop(first), again = afx_instance_stop(first);
    expected_ends += !activated;
    check("stop behind activate", !activated && !stopped && !again && wait_ended(expected_ends, 1000));

    /* With every channel in use, a stop frees them for an activation at once. */
    memcpy(wide_file, flow_file, sizeof(wide_file));
    afx_write32(wide_file + offsetof(afx_file_header_t, required_channels), 16);
    afx_asset_t wide;
    if ((result = afx_bank_flow_upload(&bank, wide_file, sizeof(wide_file), &wide))) {
        printf("AFX_CMD wide=%d\n", result);
        afx_shutdown();
        return 1;
    }
    afx_instance_t held[4], extra = 0;
    unsigned count = 0;
    while (count < 4 && activate(wide, &held[count]) == AFX_OK) ++count;
    expected_ends += count;
    int settled = count && running(held[count - 1]);
    int full = afx_instance_activate(wide, &extra);
    int reused = count ? afx_instance_stop(held[0]) : -1;
    if (!reused) reused = afx_instance_activate(wide, &held[0]);
    expected_ends += !reused;
    check("channels back at stop", count == 4 && settled && full == -AFX_NO_CHANNELS && reused == AFX_OK);
    afx_instance_t replaced = 0;
    int replace = afx_instance_replace(held[1], wide, &replaced);
    expected_ends += !replace;
    if (!replace) held[1] = replaced;
    check("replace with every channel in use", replace == AFX_OK);
    printf("AFX_CMD instances=%u full=%d reused=%d replace=%d\n", count, full, reused, replace);

    /* Released flows and their bank outlive the release until the instances end. */
    int flow_release = afx_asset_release(flow) | afx_asset_release(wide);
    int bank_release = afx_asset_release(bank.asset);
    int refused = afx_instance_activate(wide, &extra);
    int still_there = afx_asset_addr(wide) && afx_asset_addr(bank.asset);
    for (unsigned i = 0; i < count; ++i) (void)afx_instance_stop(held[i]);
    int all_ended = wait_ended(expected_ends, 3000);
    check("release when idle", !flow_release && !bank_release && refused == -AFX_INVALID_HANDLE &&
          still_there && all_ended && !afx_asset_addr(flow) && !afx_asset_addr(wide) &&
          !afx_asset_addr(bank.asset));
    printf("AFX_CMD ends=%u expected=%u\n", ended_count, expected_ends);

    /* PATCH right after ACTIVATE, before the first NOTE: that NOTE keeps it.
     * The AICA's channel registers read back over G2; an unpatched instance
     * on the next channels shows whether they do. */
    uint32_t note_tick = 0, next_tick = 0;
    uint16_t authored = 0;
    if ((result = load(&bank, &flow)) || !channel0_note(&header, &note_tick, &authored, &next_tick)) {
        printf("AFX_CMD reload=%d\n", result);
        check("patch before note", 0);
    } else {
        uint16_t patched = authored ^ 0x0007u; /* another pan, same send level */
        afx_instance_t probe, control;
        int a = afx_instance_activate(flow, &probe);
        int p = afx_instance_patch(probe, 0, 1u << AFX_FIELD_DIRECT, &patched);
        int c = afx_instance_activate(flow, &control);
        uint32_t start = afx_instance_start_tick(probe);
        /* Two ticks past the NOTE, before the channel's next one; the start
         * tick lies ahead when activation returns. */
        while ((int32_t)(afx_status_timer_ticks() - start) < (int32_t)note_tick + 2) {}
        uint16_t probe_direct = direct_register(0);
        uint16_t control_direct = direct_register(header.required_channels);
        int32_t read_at = (int32_t)(afx_status_timer_ticks() - start);
        printf("AFX_CMD direct authored=%04x patched=%04x probe=%04x control=%04x note=%lu next=%lu read=%ld\n",
               authored, patched, probe_direct, control_direct, (unsigned long)note_tick,
               (unsigned long)next_tick, (long)read_at);
        if (control_direct != authored) {
            printf("AFX_CMD inconclusive: the control channel did not read back its authored DIRECT\n");
        } else {
            check("patch before note", !a && !p && !c && (uint32_t)read_at < next_tick &&
                  probe_direct == patched);
        }
        (void)afx_instance_stop(probe);
        (void)afx_instance_stop(control);
        expected_ends += 2;
        (void)wait_ended(expected_ends, 1000);
        (void)afx_asset_release(flow);
        (void)afx_asset_release(bank.asset);
    }
    afx_shutdown();
    printf("AFX_CMD %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
