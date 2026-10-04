"""Check the tuner's real duration reader against the portable AFX codec."""
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

root = Path(__file__).resolve().parents[2]
source = (root / "tools/tuner/server/main.c").read_text()
reader = source[source.index("static int bank_flow_properties("):
                source.index("static void playback_progress(")]
harness = r'''
#include <aicaflow/codec.h>
#include <assert.h>
#include <stdio.h>
typedef struct {
    uint32_t playback_ticks, playback_tick_rate_num, playback_tick_rate_den;
    uint8_t lane_count;
} tuner_state_t;
'''
checks = r'''
int main(void) {
    uint8_t file[160] = {0};
    afx_write32(file, AFX_FILE_MAGIC); afx_write32(file + 4, AFX_FILE_VERSION);
    afx_write32(file + 8, sizeof(file));
    afx_write32(file + 16, 96); afx_write32(file + 20, 64);
    afx_write32(file + 24, AFX_SETUP_BYTES); afx_write32(file + 28, 4);
    afx_write32(file + 32, 1); afx_write32(file + 36, 1);
    afx_write32(file + 40, 1);
    afx_write32(file + 48, 80); afx_write32(file + 52, 1);
    afx_write32(file + 64, 1); afx_write32(file + 68, 1002); afx_write32(file + 72, 1);
    afx_write32(file + 88, 64);
    file[132] = AFX_OP_WAIT16; afx_write16(file + 133, 20040);
    file[135] = AFX_OP_END;
    tuner_state_t state = {0};
    assert(bank_flow_properties(file, sizeof(file), &state) == AFX_OK);
    assert(state.playback_ticks == 20040);
    assert(playback_milliseconds(&state, state.playback_ticks) == 20000);
    file[132] = 0xff;
    assert(bank_flow_properties(file, sizeof(file), &state) < 0);
    puts("Tuner duration: PASS");
}
'''
with TemporaryDirectory() as directory:
    test = Path(directory) / "properties.c"
    binary = Path(directory) / "properties"
    test.write_text(harness + reader + checks)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root / "driver/format/include"), str(test),
                    str(root / "driver/format/src/codec.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
