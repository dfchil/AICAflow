#ifndef AICAFLOW_PAN_H
#define AICAFLOW_PAN_H

#include <stdint.h>

/* Pan positions on the AICA's DIPAN.
 *
 * DIPAN attenuates one side in 3 dB steps: bits 0-3 are the steps (0..15) and
 * bit 4 picks the side, set for left (negative). A straight line from a pan
 * position to steps is not what other engines mean by pan: libultra, most
 * mixers and the equal-power law give left = cos(pi/2 * x) and right =
 * sin(pi/2 * x) for x from 0 (left) to 1 (right). Their level difference,
 * 20*log10(tan(pi/2 * x)), is small near the centre and steep at the edges:
 * position 80 of 0..127 is one step, where a straight line gives four.
 *
 * afx_pan_steps() gives that difference rounded to DIPAN steps, without
 * floating point. Both centre encodings (0 and 16) mean no attenuation. */

/* Where 20*log10(tan(pi/2 * x)) passes 3 * (k - 0.5) dB, k = 1..15, as x in
 * Q24: the positions at which a right pan reaches k steps. */
static const uint32_t afx_pan_step_q24[15] = {
    9306304u, 11039286u, 12514874u, 13678580u, 14552598u, 15190898u, 15650061u,
    15977772u, 16210726u, 16375984u, 16493099u, 16576053u, 16634795u, 16676387u,
    16705833u,
};

/* The equal-power level difference of position (0 = left, range = right) as
 * signed DIPAN steps, -15..15. range must be nonzero; a position past range
 * counts as range. */
static inline int afx_pan_steps(uint32_t position, uint32_t range) {
    if (position > range) position = range;
    int right = 2ull * position >= range;
    uint64_t away = right ? position : range - position;
    int steps = 0;
    for (int k = 0; k < 15; ++k) steps += (away << 24) >= (uint64_t)afx_pan_step_q24[k] * range;
    return right ? steps : -steps;
}

/* Signed steps from a DIPAN value (low 5 bits). */
static inline int afx_dipan_steps(uint16_t dipan) {
    return dipan & 16u ? -(int)(dipan & 15u) : (int)(dipan & 15u);
}

/* dipan moved by steps, clamped to -15..15; bits above the low 5 are kept,
 * so this works on a whole DISDL/DIPAN register word. */
static inline uint16_t afx_dipan_add(uint16_t dipan, int steps) {
    steps += afx_dipan_steps(dipan);
    if (steps < -15) steps = -15;
    if (steps > 15) steps = 15;
    return (uint16_t)((dipan & ~31u) | (steps < 0 ? 16u | (uint16_t)-steps : (uint16_t)steps));
}

#endif
