# Multiple DSP effects demo

An enDjinn on-screen audition of one AICA DSP program with two independent
paths. It plays: echo alone on the left (`MIXS0`), distortion alone on the
right (`MIXS1`), then both together. The source direct paths are muted, so the
two DSP returns are unambiguous.

## Signal flow

| Source | Voice DSP send | DSP input | DSP work | Return |
| --- | --- | --- | --- | --- |
| Short click | `0xf0`: full level, bus 0 | `MIXS0` | 180 ms packed-float delay with feedback (steps 1–9) | left / `EFREG0` |
| Looping sine | `0xf1`: full level, bus 1 | `MIXS1` | four saturating 16× stages (steps 16–26) | right / `EFREG1` |

The high nibble of the send byte is `IMXL` (send level); the low nibble is
`ISEL` (the `MIXS` bus). Both zones set `DIRECT=0`, which mutes their dry
voice output. What is heard is therefore only the DSP return.

The generated flow plays clicks at 0.5, 1.0, 1.5 and 2.0 seconds, then a sine
from 3–5 seconds. At 6 seconds it starts both again: clicks at 6.0, 6.5 and
7.0 seconds, with the sine running to 8 seconds.

```sh
source ~/projects/dreamcast/enDjinn/environ.sh
make -C examples/multiple_dsp_effects
kos-tool -f -t 10.0.0.184 -x "$PWD/examples/multiple_dsp_effects/bin/aicaflow_multiple_dsp_effects.elf"
```

Press A to replay after the three phases finish; `START+A+B+X+Y` exits. The
program uses 27 of AICA's 128 DSP steps, with separate `MIXS` inputs and a
shared scene.
