/* Hardware check of a looping flow (make -C driver loop-check): by default the
 * quickstart fixture's three notes with LOOP in place of END; any looping
 * AFX/AFB pair put in build/loop.afx and build/loop.afb works, such as one
 * from AICAforge's afx_compile --loop-tick. Every loop is reported by the loop
 * callback; the hardware ticks between loops must be the loop body's
 * authored length every time, since the loop moves only the cursor.
 * Then build/repeat.afx: the same notes played three times by a REPEAT
 * before the LOOP (firmware ABI 14), so each loop must last three times as
 * long, to the tick. */
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
alignas(32) static const unsigned char repeat_file[] = {
#embed "../build/repeat.afx"
};
alignas(32) static const unsigned char bank_file[] = {
#embed "../build/loop.afb"
};

#define LOOPS_WANTED 4
static volatile unsigned loops_seen;
static uint32_t loop_deadline[LOOPS_WANTED + 2], loop_position[LOOPS_WANTED + 2], loop_count[LOOPS_WANTED + 2];
static uint64_t loop_ms[LOOPS_WANTED + 2];
static void on_loop(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    (void)instance; (void)user;
    unsigned n = loops_seen;
    if (n < LOOPS_WANTED + 2) {
        loop_deadline[n] = status->next_deadline;
        loop_position[n] = status->position;
        loop_count[n] = status->loops;
        loop_ms[n] = timer_ms_gettime64();
    }
    loops_seen = n + 1;
}
static volatile int ended;
static afx_instance_status_t end_status;
static void on_end(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    (void)instance; (void)user;
    end_status = *status;
    ended = 1;
}

/* Plays flow_file until it has looped `wanted` times, then stops it. Returns
 * zero when every loop came at the authored length, the first one included. */
static int check_flow(const char *name, const unsigned char *flow_file, uint32_t size, unsigned wanted) {
    afx_asset_t flow;
    afx_instance_t instance = 0;
    afx_bank_t bank = {0};
    uint64_t ticks;
    uint32_t num, den, loop_tick = 99;
    afx_file_header_t header = {0};
    (void)afx_file_validate_layout(flow_file, size, &header);
    loops_seen = 0;
    ended = 0;
    int result = 0;
    if (afx_flow_duration(flow_file, size, &ticks, &num, &den)) ticks = 0;
    if (!result) result = afx_bank_load_memory(&bank, bank_file, sizeof(bank_file));
    if (!result) result = afx_bank_flow_upload(&bank, flow_file, size, &flow);
    if (!result && (!afx_asset_loops(flow, &loop_tick) || loop_tick != header.loop_tick)) result = -100;
    /* The stream's LOOP target, for the position the loop reports. */
    const uint8_t *stream = flow_file + header.image_offset + header.stream_offset;
    uint32_t target = header.stream_size >= 5 ? afx_read32(stream + header.stream_size - 4) : 0;
    printf("AFX_LOOP %s loaded=%d authored_ticks=%lu rate=%lu/%lu loop_tick=%lu\n", name, result,
           (unsigned long)ticks, (unsigned long)num, (unsigned long)den, (unsigned long)loop_tick);
    if (!result) result = afx_instance_activate(flow, &instance);
    uint32_t start_tick = result ? 0 : afx_instance_start_tick(instance);
    uint64_t start = timer_ms_gettime64();
    while (!result && loops_seen < wanted && !ended && timer_ms_gettime64() - start < 120000)
        thd_sleep(10);
    afx_instance_status_t status;
    if (!result && afx_instance_status(instance, &status) == AFX_OK)
        printf("AFX_LOOP %s status state=%lu loops=%lu position=%lu\n", name, (unsigned long)status.state,
               (unsigned long)status.loops, (unsigned long)status.position);
    if (!result && ended) result = -101; /* A looping flow must not end by itself. */
    if (!result) result = afx_instance_stop(instance);
    start = timer_ms_gettime64();
    while (!result && !ended && timer_ms_gettime64() - start < 2000) thd_sleep(10);
    if (!result && !ended) result = -102;
    unsigned seen = loops_seen < LOOPS_WANTED + 2 ? loops_seen : LOOPS_WANTED + 2;
    int steady = seen >= wanted;
    for (unsigned i = 0; i < seen; ++i) {
        uint32_t dt = loop_deadline[i] - (i ? loop_deadline[i - 1] : start_tick);
        printf("AFX_LOOP %s loop=%lu position=%lu deadline=%lu dticks=%lu dms=%llu\n", name,
               (unsigned long)loop_count[i], (unsigned long)loop_position[i],
               (unsigned long)loop_deadline[i], (unsigned long)dt,
               (unsigned long long)(i ? loop_ms[i] - loop_ms[i - 1] : 0));
        /* The reported position is image-relative: the stream start plus the target. */
        if (loop_count[i] != i + 1 || loop_position[i] != header.stream_offset + target) steady = 0;
        /* 256/256 tempo: one hardware tick per authored tick, the whole stream
         * to the first loop and the loop body to each after it. */
        if (dt != (i ? ticks - loop_tick : ticks)) steady = 0;
    }
    if (!result && !steady) result = -103;
    if (!result) result = afx_asset_free(flow);
    if (!result) result = afx_bank_release(&bank);
    printf("AFX_LOOP %s %s (%d) loops=%u ended_state=%lu\n", name, result ? "FAIL" : "PASS", result,
           loops_seen, (unsigned long)end_status.state);
    return result;
}

int main(void) {
    int result = afx_init(firmware, sizeof(firmware));
    printf("AFX_LOOP init=%d\n", result);
    if (result) return 1;
    afx_set_end_callback(on_end, NULL);
    afx_set_loop_callback(on_loop, NULL);
    int failed = check_flow("loop", flow_file, sizeof(flow_file), LOOPS_WANTED) != 0;
    failed += check_flow("repeat", repeat_file, sizeof(repeat_file), 2) != 0;
    afx_shutdown();
    printf("AFX_LOOP %s (%d failed)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
