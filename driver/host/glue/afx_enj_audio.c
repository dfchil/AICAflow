/* Attaches AICAflow's host backend to enDjinn's audio output (pc-enDjinn and
 * web-enDjinn) when afx_init() first starts the firmware. host.mk links this
 * object into an enDjinn application; nothing in the program calls it. */
#include <aicaflow/host_audio.h>
#include <enDjinn/enj_host_audio.h>

#include <stdio.h>

static void render(int16_t *stereo, uint32_t frames, void *user) {
    (void)user;
    afx_host_audio_render(stereo, frames);
}

static void attach(void) {
    if (enj_host_audio_source_set(render, NULL))
        fprintf(stderr, "AICAflow: no audio device; flows advance with the clock, unheard\n");
}

__attribute__((constructor)) static void install(void) {
    afx_host_audio_set_start_hook(attach);
}
