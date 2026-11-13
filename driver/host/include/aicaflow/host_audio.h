/* AICAflow on a PC or in a browser: the software AICA's output.
 *
 * The host build runs the Dreamcast's SH4 library and the firmware's executor
 * from the same sources, with a software AICA in place of the chip. Programs
 * use the ordinary API (<aicaflow/host.h> and the rest); this header adds the
 * audio output. While something renders it is the AICA's clock; otherwise the
 * chip follows the wall clock. See docs/design/host-backend.md. */
#ifndef AICAFLOW_HOST_AUDIO_H
#define AICAFLOW_HOST_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The output rate in Hz: 44100, as on the console. */
uint32_t afx_host_audio_rate(void);

/* Fills frames of interleaved 16-bit stereo. Any thread; one renderer at a
 * time. Before afx_init() it renders silence. */
void afx_host_audio_render(int16_t *stereo, uint32_t frames);

/* Called once when the firmware first starts (from afx_init()), so an audio
 * output can attach itself; NULL removes it. enDjinn's glue (driver/host/host.mk)
 * sets this before main(). */
void afx_host_audio_set_start_hook(void (*hook)(void));

#ifdef __cplusplus
}
#endif

#endif
