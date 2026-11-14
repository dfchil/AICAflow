/* Hardware check of songs (make -C driver song-check), with the quickstart
 * fixture's notes as one section, looping (tests/make_fixtures.py):
 * - song.afm continues on the same executor and voices at its NEXT, as a LOOP
 *   flow did: every loop must come the section's 8160 ticks after the last;
 * - song_new.afm starts each play on the song's other executor 1000 ticks
 *   before the section's end, while the old play rings out: every loop must
 *   come 7160 ticks after the last, the first 7160 after activation.
 * Then, on the second: a restart while the last instance still stops, a pause
 * resumed through afx_instance_rebuild at the reported position, and every
 * runtime block given back. */
#include <kos.h>
#include <aicaflow/bank.h>
#include <aicaflow/host.h>
#include <aicaflow/codec.h>
#include <aicaflow/song.h>
#include <stdio.h>
#include <stdalign.h>

alignas(32) static const unsigned char firmware[] = {
#embed "../../firmware/aicaflow.drv"
};
alignas(32) static const unsigned char continued[] = {
#embed "../build/song.afm"
};
alignas(32) static const unsigned char renewed[] = {
#embed "../build/song_new.afm"
};
alignas(32) static const unsigned char bank_file[] = {
#embed "../build/long.afb"
};

#define LOOPS_KEPT 8
static volatile unsigned loops_seen;
static uint32_t loop_deadline[LOOPS_KEPT], loop_position[LOOPS_KEPT], loop_count[LOOPS_KEPT];
static void on_loop(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    (void)instance; (void)user;
    unsigned n = loops_seen;
    if (n < LOOPS_KEPT) {
        loop_deadline[n] = status->next_deadline;
        loop_position[n] = status->position;
        loop_count[n] = status->loops;
    }
    loops_seen = n + 1;
}
static volatile unsigned ended;
static void on_end(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    (void)instance; (void)status; (void)user;
    ++ended;
}
static int wait_until(volatile unsigned *counter, unsigned count, unsigned ms) {
    uint64_t start = timer_ms_gettime64();
    while (*counter < count && timer_ms_gettime64() - start < ms) thd_sleep(10);
    return *counter >= count;
}
static int failures;
static void check(const char *name, int ok) {
    printf("AFX_SONG %s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) ++failures;
}

/* Plays a song until it has looped `wanted` times and stops it; every loop
 * must come body ticks after the last, the first body ticks after activation. */
static int play(const char *name, afx_asset_t song, uint32_t body, unsigned wanted) {
    afx_instance_t instance = 0;
    unsigned end_before = ended;
    loops_seen = 0;
    if (afx_instance_activate(song, &instance)) return 0;
    uint32_t start = afx_instance_start_tick(instance);
    int looped = wait_until(&loops_seen, wanted, 60000);
    int stopped = !afx_instance_stop(instance) && wait_until(&ended, end_before + 1, 2000);
    int steady = looped && stopped;
    for (unsigned i = 0; i < wanted && i < LOOPS_KEPT; ++i) {
        uint32_t dt = loop_deadline[i] - (i ? loop_deadline[i - 1] : start);
        printf("AFX_SONG %s loop=%lu position=%08lx dticks=%lu\n", name, (unsigned long)loop_count[i],
               (unsigned long)loop_position[i], (unsigned long)dt);
        steady &= dt == body && loop_count[i] == i + 1 && loop_position[i] == AFX_SONG_POSITION(0, 0);
    }
    return steady;
}

int main(void) {
    afx_bank_t bank = {0};
    afx_asset_t song = 0, pooled = 0;
    afx_mem_stats_t base, now;
    int result = afx_init(firmware, sizeof(firmware));
    printf("AFX_SONG init=%d\n", result);
    if (result) return 1;
    afx_set_end_callback(on_end, NULL);
    afx_set_loop_callback(on_loop, NULL);
    const afx_bank_t *banks[1] = {&bank};
    result = afx_bank_load_memory(&bank, bank_file, sizeof(bank_file));
    if (!result) result = afx_song_load(banks, 1, continued, sizeof(continued), &song);
    if (!result) result = afx_song_load(banks, 1, renewed, sizeof(renewed), &pooled);
    printf("AFX_SONG loaded=%d\n", result);
    if (result) { afx_shutdown(); return 1; }
    (void)afx_mem_stats(&base);

    check("a continue join loops every section, on the same voices", play("continue", song, 8160, 4));
    check("a new-play join starts the next play as the last rings out", play("new-play", pooled, 7160, 3));

    /* A restart while the last instance still stops: each has its own block. */
    afx_instance_t first = 0, second = 0;
    unsigned end_before = ended;
    int a = afx_instance_activate(pooled, &first);
    thd_sleep(100);
    int b = afx_instance_stop(first);
    int c = afx_instance_activate(pooled, &second);
    thd_sleep(500);
    afx_instance_status_t status;
    int running = !afx_instance_status(second, &status) && (status.state & 0xffu) == AFX_RUNNING;
    printf("AFX_SONG restart activate=%d stop=%d activate=%d state=%lu\n", a, b, c,
           (unsigned long)status.state);
    check("a song restarts while its last instance stops", !a && !b && !c && running &&
          wait_until(&ended, end_before + 1, 2000));

    /* Pause, then resume at the reported position with the remaining wait. */
    thd_sleep(1500);
    loops_seen = 0;
    int paused = !afx_instance_pause(second);
    thd_sleep(50);
    int read = !afx_instance_status(second, &status) && (status.state & 0xffu) == AFX_PAUSED;
    uint32_t position = status.position;
    int resumed = read && !afx_instance_rebuild(second, NULL, 0, position, 0,
                                                afx_status_timer_ticks() + 1u, true);
    thd_sleep(50);
    int playing = !afx_instance_status(second, &status) && (status.state & 0xffu) == AFX_RUNNING;
    printf("AFX_SONG pause=%d status=%d position=%08lx rebuild=%d state=%lu\n", paused, read,
           (unsigned long)position, resumed, (unsigned long)status.state);
    int looped = wait_until(&loops_seen, 1, 10000);
    end_before = ended;
    int stopped = !afx_instance_stop(second) && wait_until(&ended, end_before + 1, 2000);
    check("a paused song resumes where it stopped and loops on", paused && resumed && playing &&
          (position >> AFX_SONG_POSITION_SHIFT) == 0 && looped && stopped);

    thd_sleep(200);
    (void)afx_mem_stats(&now);
    check("every runtime block is given back", now.used_bytes == base.used_bytes);
    printf("AFX_SONG used=%lu/%lu\n", (unsigned long)now.used_bytes, (unsigned long)base.used_bytes);
    (void)afx_asset_free(song);
    (void)afx_asset_free(pooled);
    (void)afx_bank_release(&bank);
    afx_shutdown();
    printf("AFX_SONG %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
