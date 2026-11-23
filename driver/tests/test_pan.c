#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "aicaflow/pan.h"

/* The equal-power law in floating point: the level difference between the
 * sides, rounded to 3 dB steps. */
static int reference(uint32_t position, uint32_t range) {
    if (position == 0) return -15;
    if (position >= range) return 15;
    double db = 20.0 * log10(tan(M_PI / 2.0 * (double)position / (double)range));
    long steps = lround(db / 3.0);
    return steps < -15 ? -15 : steps > 15 ? 15 : (int)steps;
}

int main(void) {
    for (uint32_t range = 1; range <= 1024; ++range)
        for (uint32_t position = 0; position <= range; ++position)
            assert(afx_pan_steps(position, range) == reference(position, range));
    for (uint32_t position = 0; position <= 65535; ++position)
        assert(afx_pan_steps(position, 65535) == reference(position, 65535));
    assert(afx_pan_steps(300, 255) == 15);

    /* libultra's 0..127: the centre is 64 (63.5 rounds to no steps), 80 is one step. */
    assert(afx_pan_steps(64, 127) == 0 && afx_pan_steps(63, 127) == 0);
    assert(afx_pan_steps(80, 127) == 1 && afx_pan_steps(0, 127) == -15);

    assert(afx_dipan_steps(0) == 0 && afx_dipan_steps(16) == 0);
    assert(afx_dipan_steps(0x13) == -3 && afx_dipan_steps(5) == 5);
    assert(afx_dipan_add(0, -3) == 0x13 && afx_dipan_add(0x13, 3) == 0);
    assert(afx_dipan_add(12, 9) == 15 && afx_dipan_add(0x1c, -9) == 0x1f);
    assert(afx_dipan_add(0x0f00 | 0x12, 4) == (0x0f00 | 2));
    puts("pan checks passed");
    return 0;
}
