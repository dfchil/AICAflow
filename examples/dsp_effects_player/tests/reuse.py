"""Compile the real playback functions against counters; no Dreamcast needed."""
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

source = (Path(__file__).resolve().parents[1] / "code/main.c").read_text()
playback = source[source.index("static int unload(void)"):source.index("static void render(void")]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef int afx_dsp_program_t;
typedef struct { int state; } afx_instance_status_t;
enum { AFX_OK, AFX_DONE, AFX_ERROR, AFX_RUNNING, AFX_TIMEOUT };
static int selected, input, installed_effect = -1, playing_effect = -1;
static int instance, flows[] = {1, 2}, uploads, fail_upload, fail_activate;
static bool running, returns_enabled = true, audible;
static char message[80];
static struct { const char *name, *preset; } effects[] = {{"A", "a"}, {"B", "b"}};
static struct { const char *name, *stem; } inputs[] = {{"one", "one"}, {"two", "two"}};
static unsigned selected_input(void) { return input; }
static int afx_instance_status(int i, afx_instance_status_t *s) { (void)i; s->state = AFX_RUNNING; return 0; }
static int afx_instance_stop(int i) { (void)i; return 0; }
static int wait_for(int i, int state) { (void)i; (void)state; return 0; }
static int afx_instance_recycle(int i) { (void)i; return 0; }
static int wait_recycled(int i) { (void)i; return 0; }
static int afx_dsp_scene_returns(bool enabled) { audible = enabled; return 0; }
static int afx_dsp_scene_disable(void) { audible = false; return 0; }
static int afx_dsp_program_demo(int *p, const char *name) { (void)name; *p = 1; return 0; }
static int afx_dsp_scene_program(const void *p, unsigned size) {
    (void)p; (void)size; ++uploads; return fail_upload;
}
static int afx_instance_activate(int flow, int *i) { *i = flow; return fail_activate; }
'''
checks = r'''
int main(void) {
    assert(play() == 0 && uploads == 1 && audible);
    assert(play() == 0 && uploads == 1); /* Repeat while playing. */
    input = 1;
    assert(play() == 0 && uploads == 1 && instance == 2);
    assert(unload() == 0 && !audible);
    assert(play() == 0 && uploads == 1); /* Stop/end then replay. */
    returns_enabled = false;
    assert(play() == 0 && uploads == 1 && !audible);
    selected = 1;
    assert(play() == 0 && uploads == 2);
    selected = 0; fail_upload = -1;
    assert(play() == -1 && installed_effect == -1 && !audible);
    fail_upload = 0;
    assert(play() == 0 && uploads == 4 && installed_effect == 0);
    fail_activate = -1;
    assert(play() == -1 && uploads == 4);
    fail_activate = 0;
    assert(play() == 0 && uploads == 4);
    puts("DSP program reuse: PASS");
}
'''
with TemporaryDirectory() as directory:
    test = Path(directory) / "reuse.c"
    binary = Path(directory) / "reuse"
    test.write_text(harness + playback + checks)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
