/* Hardware check of a looping flow (make -C driver loop-check): by default the
 * quickstart fixture's three notes with LOOP in place of END; any looping
 * AFX/AFB pair put in build/loop.afx and build/loop.afb works, such as one
 * from AICAforge's afx_compile --loop-tick. Every loop is reported by the loop
 * callback; the hardware ticks between loops must be the loop body's
 * authored length every time, since the loop moves only the cursor. */
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

int main(void) {
    afx_asset_t flow;
    afx_instance_t instance;
    afx_bank_t bank = {0};
    uint64_t ticks;
    uint32_t num, den, loop_tick = 99;
    afx_file_header_t header = {0};
    (void)afx_file_validate_layout(flow_file, sizeof(flow_file), &header);
    int result = afx_init(firmware, sizeof(firmware));
    printf("AFX_LOOP init=%d\n", result);
    if (result) return 1;
    afx_set_end_callback(on_end, NULL);
    afx_set_loop_callback(on_loop, NULL);
    if (afx_flow_duration(flow_file, sizeof(flow_file), &ticks, &num, &den)) ticks = 0;
    if (!result) result = afx_bank_load_memory(&bank, bank_file, sizeof(bank_file));
    if (!result) result = afx_bank_flow_upload(&bank, flow_file, sizeof(flow_file), &flow);
    if (!result && (!afx_asset_loops(flow, &loop_tick) || loop_tick != header.loop_tick)) result = -100;
    /* The stream's LOOP target, for the position the loop reports. */
    const uint8_t *stream = flow_file + header.image_offset + header.stream_offset;
    uint32_t target = header.stream_size >= 5 ? afx_read32(stream + header.stream_size - 4) : 0;
    printf("AFX_LOOP loaded=%d authored_ticks=%lu rate=%lu/%lu loop_tick=%lu\n", result,
           (unsigned long)ticks, (unsigned long)num, (unsigned long)den, (unsigned long)loop_tick);
    if (!result) result = afx_instance_activate(flow, &instance);
    uint64_t start = timer_ms_gettime64();
    while (!result && loops_seen < LOOPS_WANTED && !ended && timer_ms_gettime64() - start < 60000)
        thd_sleep(10);
    afx_instance_status_t status;
    if (!result && afx_instance_status(instance, &status) == AFX_OK)
        printf("AFX_LOOP status state=%lu loops=%lu position=%lu\n", (unsigned long)status.state,
               (unsigned long)status.loops, (unsigned long)status.position);
    if (!result && ended) result = -101; /* A looping flow must not end by itself. */
    if (!result) result = afx_instance_stop(instance);
    start = timer_ms_gettime64();
    while (!result && !ended && timer_ms_gettime64() - start < 2000) thd_sleep(10);
    if (!result && !ended) result = -102;
    unsigned seen = loops_seen < LOOPS_WANTED + 2 ? loops_seen : LOOPS_WANTED + 2;
    int steady = seen >= LOOPS_WANTED;
    for (unsigned i = 0; i < seen; ++i) {
        uint32_t dt = i ? loop_deadline[i] - loop_deadline[i - 1] : 0;
        printf("AFX_LOOP loop=%lu position=%lu deadline=%lu dticks=%lu dms=%llu\n",
               (unsigned long)loop_count[i], (unsigned long)loop_position[i],
               (unsigned long)loop_deadline[i], (unsigned long)dt,
               (unsigned long long)(i ? loop_ms[i] - loop_ms[i - 1] : 0));
        /* The reported position is image-relative: the stream start plus the target. */
        if (loop_count[i] != i + 1 || loop_position[i] != header.stream_offset + target) steady = 0;
        /* 256/256 tempo: one hardware tick per authored tick of the loop body. */
        if (i && dt != ticks - loop_tick) steady = 0;
    }
    if (!result && !steady) result = -103;
    if (!result) result = afx_asset_free(flow);
    if (!result) result = afx_bank_release(&bank);
    afx_shutdown();
    printf("AFX_LOOP %s (%d) loops=%u ended_state=%lu\n", result ? "FAIL" : "PASS", result, loops_seen,
           (unsigned long)end_status.state);
    return result ? 1 : 0;
}
