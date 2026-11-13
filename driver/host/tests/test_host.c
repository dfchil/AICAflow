/* Host backend: the real firmware and SH4 library on the software AICA. */
#include "kos.h"
#include "aica.h"

#include <aicaflow/bank.h>
#include <aicaflow/host.h>
#include <aicaflow/host_audio.h>

#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

alignas(32) static const unsigned char firmware[] = {
#embed "../../../firmware/aicaflow.drv"
};
alignas(32) static const unsigned char flow_file[] = {
#embed "../../build/loop.afx"
};
alignas(32) static const unsigned char bank_file[] = {
#embed "../../build/loop.afb"
};

static volatile int ended;
static void on_end(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    (void)instance; (void)status; (void)user;
    ended = 1;
}

int main(void) {
    assert(afx_init(firmware, sizeof(firmware)) == AFX_OK);
    afx_bank_t bank = {0};
    afx_asset_t flow;
    afx_instance_t instance;
    assert(afx_bank_load_memory(&bank, bank_file, sizeof(bank_file)) == AFX_OK);
    assert(afx_bank_flow_upload(&bank, flow_file, sizeof(flow_file), &flow) == AFX_OK);
    afx_set_end_callback(on_end, NULL);
    assert(afx_instance_activate(flow, &instance) == AFX_OK);
    static int16_t audio[2 * 44100];
    afx_host_audio_render(audio, 44100);
    double energy = 0;
    int peak = 0;
    for (unsigned i = 0; i < 2 * 44100; ++i) {
        energy += (double)audio[i] * audio[i];
        if (abs(audio[i]) > peak) peak = abs(audio[i]);
    }
    double rms = sqrt(energy / (2 * 44100));
    printf("one second of the loop fixture: rms %.0f peak %d\n", rms, peak);
    assert(rms > 100 && peak < 32767);
    assert(afx_instance_stop(instance) == AFX_OK);
    for (int waited = 0; !ended && waited < 200; ++waited) afx_test_sleep(10);
    assert(ended);
    int freed = afx_asset_free(flow);
    printf("free after the end callback: %d\n", freed);
    assert(freed == AFX_OK);
    assert(afx_bank_release(&bank) == AFX_OK);
    afx_shutdown();
    puts("host backend checks passed");
    return 0;
}
