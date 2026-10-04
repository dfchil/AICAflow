# DSP effects player

[Documentation](../../docs/README.md)

An interactive enDjinn Dreamcast listener for nine selected DSP programs:
Echo, Room, Warm room, Chorus, Flanger, Resonant filter,
Ping-pong, Multitap and Large room.
It constructs the selected program in C at runtime and applies it to an input
from the resident sample bank. Other prefabs remain available through the API.

All samples share `inputs.afb`. The bank and all six AFX flows are uploaded
once at startup and remain in AICA RAM until exit. Playing or switching effects
does not read files. Each flow leaves at least one second for the DSP tail.
Replaying the same effect, including with another input, reuses the installed
DSP program and delay memory. Stop mutes its returns; a different effect replaces
the scene. The delay memory is not cleared on replay.

| Input | Audio / control voices |
| --- | --- |
| Control phrase | C5–E5–G5–C6 phrase on MIXS0; notes 24, 31, 36, 43 on MIXS1–4 |
| Impulse | Short C7 sine burst on MIXS0 |
| Tone | Short A4 sine on MIXS0 |
| Modulated tone | The same phrase; note 36 on MIXS1 only |
| Wilhelm scream | Original mono 22,050 Hz sample on MIXS0 |
| Wilhelm + slow LFO | Wilhelm at −6 dB on MIXS0; 0.5 Hz sine on MIXS1, no direct output |

“Effect-tuned” selects impulse for echo/rooms, Wilhelm for the resonant filter,
and Wilhelm with a slow LFO for chorus/flanger.
The selected source is shown on screen. Press A to start it after selecting.

Control voices have no direct output. The original five input choices remain
available manually. Their audio-rate sine controls are not suitable as a slow
chorus LFO.

```sh
source /opt/toolchains/dc/kos/environ.sh
make -C examples/dsp_effects_player
```

Host check for replay, input changes, stop and failed program uploads:
`python3 examples/dsp_effects_player/tests/reuse.py`.

Load `examples/dsp_effects_player/bin/dsp_effects_player.elf` with the normal
Dreamcast loader.

```sh
kos-tool -t 10.0.0.184 \
  -m "$PWD/examples/dsp_effects_player/cdrom/dsp_effects_player" \
  -x "$PWD/examples/dsp_effects_player/bin/dsp_effects_player.elf"
```

- D-pad up/down selects an effect; left/right cycles automatic Effect-tuned and six manual inputs.
- A starts the selected effect.
- B stops it.
- Y switches the DSP return between wet+dry and dry.
- START+A+B+X+Y exits.

## Hardware audition

The tuner audit records dry/wet pairs from HDMI, verifies DSP upload readback,
and measures spectral change, level-normalized residual, stereo balance and tail
duration. These measurements support conservative demo selection; they do not
prove subjective audibility for every listener. The initial exclusions in
[hardware-results.json](tests/hardware-results.json) are historical; see
[isolated retests](tests/hardware-retest.json) for the current findings.
All 24 presets completed isolated tests, and all 17 excluded presets received
targeted-input tests. Correcting MADRS indexing restored the delay paths;
Ping-pong, Multitap and Large room are back in the menu. Other working effects
still need suitable demo input/mixing; they are not classified as broken.

With the tuner running on `10.0.0.184`, on a Mac with Swift and NumPy:

```sh
python3 examples/dsp_effects_player/tests/hardware_audit.py /tmp/dsp-audition
```

The recorder selects the Live Gamer HDMI device and takes its native channels
0/1 at 48 kHz. It does not downmix the device's four channels. The command resets
the tuner and reloads the bank before each effect, and saves fresh WAV pairs
plus JSON measurements. AFX/AFB files are snapshotted and their bank identities
checked before uploading.
Add `--all` to repeat the archived 24-effect audit, including excluded programs.

For a specific preset/source use `ringmod:modulated`. `--wet-only` removes
direct output from the wet flow, to distinguish a working effect from one
masked by the dry mix. Diagnostic click, driven sine, saw, LFO and ramp/window
inputs can be generated without modifying the demo bank:

```sh
mkdir -p /tmp/dsp-probes
cc -Idependencies/AICAforge/src -Idriver/format/include \
  examples/dsp_effects_player/tests/audition_inputs.c \
  dependencies/AICAforge/src/afx_compile_c.c driver/format/src/codec.c \
  -lm -o /tmp/dsp-probes/generate
/tmp/dsp-probes/generate /tmp/dsp-probes
AFX_AUDIT_ASSETS=/tmp/dsp-probes python3 \
  examples/dsp_effects_player/tests/hardware_audit.py /tmp/dsp-probe-capture \
  --wet-only ringmod:carrier highpass:saw distortion:drive
```

MADRS is indexed directly by MASA, not by twice MASA. The correction was tested
on hardware (including alternating 180 ms Ping-pong echoes), and agrees with
Flycast's [DSP interpreter](https://github.com/flyinghead/flycast/blob/master/core/hw/aica/dsp_interp.cpp).
