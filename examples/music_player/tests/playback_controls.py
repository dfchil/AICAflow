#!/usr/bin/env python3
"""Run the player's actual control helpers with a deterministic host/clock stub."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / "player_framework/music_player.c").read_text()
helpers = source[source.index("static uint16_t song_tempo("):source.index("static int start(void)")]
clock = source[source.index("static uint32_t playback_ms("):source.index("static void spectrum(")]
harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define PLAYER_SONG_TEMPO(index) (256u + (index)*128u)
#define PLAYER_SONG_GAIN(index) ((index) ? 128u : 255u)
enum { AFX_PAUSED, AFX_RUNNING };
static int playing=-1, instance=1, error;
static bool paused;
static unsigned speed_percent=100, volume_percent=100, calls, pauses, seeks;
static uint16_t tempo_q8_8=256, sent_tempo;
static uint8_t sent_gain;
static uint32_t paused_ms, started, now=10000, authored_duration_ms=60000;
static uint32_t duration_ms=60000, rate_num=1000, rate_den=1, seek_tick;
static uint32_t afx_status_timer_ticks(void) { return now; }
static int afx_instance_gain(int i, uint8_t gain) {
    assert(i==instance); ++calls; sent_gain=gain; return error;
}
static int afx_instance_tempo(int i, uint16_t tempo) {
    assert(i==instance); ++calls; sent_tempo=tempo; return error;
}
static int afx_instance_pause(int i) { assert(i==instance); ++pauses; return error; }
static int wait_state(unsigned state) { assert(state<=AFX_RUNNING); return error; }
static int afx_instance_seek_checkpoint(int i, uint32_t tick, uint32_t *selected) {
    assert(i==instance); ++seeks; seek_tick=tick; *selected=tick/1000u*1000u; return error;
}
""" + clock + helpers + r"""
int main(void) {
    /* Stopped controls clamp without touching a stale instance. */
    assert(!set_speed(500) && speed_percent==200 && !calls);
    assert(!set_speed(-1) && speed_percent==50 && !calls);
    assert(!set_volume(-1) && !volume_percent && !calls);
    assert(!set_volume(105) && volume_percent==100 && !calls);
    /* A song change composes the persistent speed with the profile tempo. */
    assert(song_tempo(0,200)==512 && song_tempo(1,200)==768);
    assert(song_tempo(0,0)==16 && song_tempo(0,10000)==4096);
    playing=0; now=10999;
    assert(!set_speed(200) && sent_tempo==512 && tempo_q8_8==512);
    assert(pauses==1 && seeks==1 && seek_tick==10999);
    assert(playback_ms()==5000 && duration_ms==30000);
    /* The same authored position is retained while paused; no seek/resume. */
    paused=true; paused_ms=5000;
    assert(!set_speed(50) && paused && paused_ms==20000);
    assert(tempo_q8_8==128 && duration_ms==120000 && seeks==1);
    assert(!set_volume(50) && sent_gain==128);
    playing=1;
    assert(!set_volume(50) && sent_gain==64);
    assert(!set_volume(0) && sent_gain==0);
    assert(!set_volume(100) && sent_gain==128);
    /* Failed IPC must not make the UI claim the new setting was applied. */
    error=-1;
    assert(set_speed(100)==-1 && speed_percent==50 && paused_ms==20000);
    assert(set_volume(50)==-1 && volume_percent==100);
    error=0;
    assert(!set_speed(100) && !set_volume(100));
    assert(speed_percent==100 && tempo_q8_8==384 && volume_percent==100);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="aica-player-controls-") as temporary:
    path = Path(temporary)
    (path / "test.c").write_text(harness)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("Player speed/volume controls passed")
