# DSP effects player

An interactive enDjinn Dreamcast listener for AICAflow's twenty-four named DSP
programs. It constructs the selected program in C at runtime and applies it to
the selected input. It has effect-tuned, impulse, tone, modulated-tone and
Wilhelm-scream sources. No `.dsp` files or archived DSP recordings are used.

The effect-tuned source contains one audible phrase on `MIXS0` plus four silent
direct control voices on `MIXS1..4`. This makes the modulation, pitch-shift
and bow presets useful to audition as well as the ordinary `MIXS0` effects.

```sh
source /opt/toolchains/dc/kos/environ.sh
source ../enDjinn/environ.sh
python3 -m pip install mido
make -C examples/dsp_effects_player
```

Load `examples/dsp_effects_player/bin/aicaflow_dsp_effects.elf` with the normal
Dreamcast loader.

- D-pad up/down selects an effect; left/right selects an input.
- A starts the selected effect.
- B stops it.
- Y switches the DSP return between wet+dry and dry.
- Start exits.
