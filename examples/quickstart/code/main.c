#include <kos.h>

#include <aicaflow/dsp.h>
#include <aicaflow/bank.h>
#include <aicaflow/host.h>

#include <stdio.h>
#include <stdalign.h>

alignas(32) static const unsigned char firmware[] = {
#embed "../../../firmware/aicaflow.drv"
};
alignas(32) static const unsigned char fixture[] = {
#embed "../build/fixture.afx"
};
alignas(32) static const unsigned char fixture_bank[] = {
#embed "../build/fixture.afb"
};

/* AICAflow reports an instance that ended from its event thread, after it has
 * freed the instance's channels. */
static volatile int ended;
static afx_instance_status_t end_status;
static void on_end(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    (void)instance; (void)user;
    end_status = *status;
    ended = 1;
}

/* Status arrives by itself; this only looks at it. */
static int wait_running(afx_instance_t instance, uint32_t timeout_ms) {
    uint64_t deadline = timer_ms_gettime64() + timeout_ms;
    do {
        afx_instance_status_t status;
        if (ended) return 1; /* Ran and ended already. */
        if (afx_instance_status(instance, &status) == AFX_OK && status.state == AFX_RUNNING) return 1;
        thd_sleep(1);
    } while (timer_ms_gettime64() < deadline);
    return 0;
}

static int wait_ended(uint32_t timeout_ms) {
    uint64_t deadline = timer_ms_gettime64() + timeout_ms;
    while (!ended && timer_ms_gettime64() < deadline) thd_sleep(1);
    return ended && end_status.state == AFX_DONE;
}

static int dsp_room_upload(void) {
    afx_dsp_program_t program;
    int result = afx_dsp_program_room(&program, 22936, 11464, false, 256, false);
    return result ? result : afx_dsp_scene_program(&program, sizeof(program));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    afx_asset_t flow;
    afx_instance_t instance;
    afx_bank_t bank = {0};
    int result = afx_init(firmware, sizeof(firmware));
    afx_set_end_callback(on_end, NULL);
    if (!result) result = afx_bank_load_memory(&bank, fixture_bank, sizeof(fixture_bank));
    if (!result) result = afx_bank_flow_upload(&bank, fixture, sizeof(fixture), &flow);
    if (!result) result = afx_instance_activate(flow, &instance);
    if (!result && !wait_running(instance, 1000)) result = -1;
    if (!result) result = dsp_room_upload();
    if (!result && !wait_ended(12000)) result = -1;
    if (!result) result = afx_asset_free(flow);
    if (!result) result = afx_bank_release(&bank);
    printf("Aicaflow quickstart: %s (%d)\n", result ? "FAIL" : "PASS", result);
    afx_shutdown();
    return result ? 1 : 0;
}
