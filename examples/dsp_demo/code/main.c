#include <kos.h>

#include <aicaflow/host.h>

#include <stdalign.h>
#include <stdio.h>

alignas(32) static const unsigned char firmware[] = {
#embed "../../../driver/arm7/aicaflow.drv"
};
alignas(32) static const unsigned char demo[] = {
#embed "../build/demo.afx"
};

static int wait_for(afx_instance_t instance, uint32_t state, uint32_t timeout_ms) {
    uint64_t deadline = timer_ms_gettime64() + timeout_ms;
    do {
        afx_instance_status_t status;
        if (afx_update() < 0 || afx_instance_status(instance, &status)) return 0;
        if (status.state == state) return 1;
        thd_sleep(1);
    } while (timer_ms_gettime64() < deadline);
    return 0;
}

static int wait_recycled(afx_instance_t instance, uint32_t timeout_ms) {
    uint64_t deadline = timer_ms_gettime64() + timeout_ms;
    do {
        afx_instance_status_t status;
        if (afx_update() < 0) return 0;
        if (afx_instance_status(instance, &status) == -AFX_STALE_GENERATION) return 1;
        thd_sleep(1);
    } while (timer_ms_gettime64() < deadline);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    afx_asset_t flow;
    afx_instance_t instance;
    afx_dsp_program_t pingpong;
    int result = afx_init(firmware, sizeof(firmware));
    if (!result) result = afx_flow_upload(demo, sizeof(demo), &flow);
    if (!result) result = afx_instance_activate(flow, &instance);
    if (!result && !wait_for(instance, AFX_RUNNING, 1000)) result = -1;
    if (!result) result = afx_dsp_program_pingpong(&pingpong, 7938, 19656);
    if (!result) result = afx_dsp_scene_program(&pingpong, sizeof(pingpong));
    if (!result) printf("Aicaflow DSP demo: ping-pong delay\n");
    if (!result && !wait_for(instance, AFX_DONE, 10000)) result = -1;
    if (!result) result = afx_instance_recycle(instance);
    if (!result && !wait_recycled(instance, 1000)) result = -1;
    if (!result) result = afx_asset_free(flow);
    printf("Aicaflow DSP demo: %s (%d)\n", result ? "FAIL" : "PASS", result);
    afx_shutdown();
    return result ? 1 : 0;
}
