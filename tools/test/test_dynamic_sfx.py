"""Exercise the demo's real pan conversion and packed register patches."""
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

root = Path(__file__).resolve().parents[2]
source = (root / "examples/dynamic_sfx/code/main.c").read_text()
pan = source[source.index("static uint8_t direct_pan("):source.index("static int make_bank_flow(")]
patch = source[source.index("static int update_engine("):source.index("static uint8_t triangle(")]
harness = r'''
#include <aicaflow/format.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t afx_instance_t;
enum { SLIDE_PAN_STEPS = 12 };
static uint16_t registers[AFX_FIELD_COUNT];
static int afx_instance_patch(afx_instance_t instance, uint8_t channel,
                              uint32_t mask, const uint16_t *values) {
    assert(instance == 1 && channel == 0);
    for (unsigned i = 0; i < AFX_FIELD_COUNT; ++i)
        if (mask & (1u << i)) registers[i] = *values++;
    return 0;
}
'''
checks = r'''
int main(void) {
    int previous = -16;
    for (unsigned pan = 0; pan <= 31; ++pan) {
        unsigned native = direct_pan(pan);
        int signed_pan = native & 16 ? -(int)(native & 15) : (int)(native & 15);
        assert(signed_pan >= previous && signed_pan - previous <= 1);
        previous = signed_pan;
    }
    assert(direct_pan(0) == 31 && direct_pan(31) == 15);
    assert(direct_pan(15) == 16 && direct_pan(16) == 0);
    assert(slide_pan(0, false) == 31 && slide_pan(12, false) == 0);
    assert(slide_pan(0, true) == 0 && slide_pan(12, true) == 31);
    configure_setup(registers, 0, 100, true, 15);
    assert(registers[AFX_FIELD_DIRECT] == 0x0f10);
    assert(registers[AFX_FIELD_FILTER_AD] == 0x1f1f);
    assert(registers[AFX_FIELD_FILTER_DR] == 0x1f1f);
    for (unsigned brightness = 0; brightness <= 15; ++brightness) {
        assert(update_engine(1, 16, 255, 0x123, 14, brightness, 0x456) == 0);
        assert(registers[AFX_FIELD_PITCH] == 0x123);
        assert(registers[AFX_FIELD_LFO] == 0x456);
        assert(registers[AFX_FIELD_DIRECT] == 0x0f00);
        assert(registers[AFX_FIELD_MIX] == 0x180e);
        for (unsigned field = AFX_FIELD_FILTER_LEVEL0; field <= AFX_FIELD_FILTER_LEVEL4; ++field)
            assert(registers[field] == 0x1500 + brightness * 0x0900 / 15);
    }
    assert(update_engine(1, 0, 0, 0, 6, 0, 0) == 0);
    assert(registers[AFX_FIELD_MIX] == 0x2c06);
    assert(mute_engine(1, 6) == 0 && registers[AFX_FIELD_MIX] == 0xff06);
    puts("Dynamic SFX pan/filter packing: PASS");
}
'''
with TemporaryDirectory() as directory:
    test = Path(directory) / "dynamic.c"
    binary = Path(directory) / "dynamic"
    test.write_text(harness + pan + patch + checks)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root / "driver/format/include"), str(test),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
