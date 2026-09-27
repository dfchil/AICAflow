# AICAflow DKR Edition 0.1.1

This is the minimal AICAflow source distribution needed by the Dreamcast port
of Diddy Kong Racing. It contains the AICA firmware, SH-4 host library, and
the N64 music/SFX import pipeline used by DKR.

It is released under the [MIT License](LICENSE).

It deliberately does **not** contain the editor, tuner, general MIDI tools,
other importers, examples, DSP labs, recordings, or unrelated documentation.

## Requirements

- KallistiOS with `kos-cc`, `arm-eabi-gcc`, and `arm-eabi-objcopy`
- Python 3.10+ and `mido` (`python3 -m pip install mido`)
- A DKR checkout with its extracted assets

## Use with DKR

Extract this archive next to the DKR checkout, then point `AICAFLOW_ROOT` at
the extracted directory:

```sh
make -f Makefile.dc -j8 AICAFLOW_ROOT=../aicaflow-dkr-0.1.0 dkracing.elf
make -f Makefile.dc -j8 AICAFLOW_ROOT=../aicaflow-dkr-0.1.0 cdi
```

The first command builds the normal dc-tool-IP ELF. The second produces a CDI
and uses the `/cd` asset mount internally.

`MANIFEST.sha256` lists every shipped source file and its SHA-256 digest.
