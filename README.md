# AICAflow DKR Edition 0.1.5

This is the minimal AICAflow source distribution needed by the Dreamcast port
of Diddy Kong Racing. It contains the AICA firmware, SH-4 host library, and
the N64 music/SFX import pipeline used by DKR.

It is released under the [MIT License](LICENSE).

It deliberately does **not** contain the editor, tuner, general MIDI tools,
other importers, examples, DSP labs, recordings, or unrelated documentation.

## Documentation

- [DKR integration](docs/INTEGRATION.md): runtime ownership and build paths.
- [N64 import](docs/N64_IMPORT.md): DKR's offline AFB/AFX asset pipeline.
- [Lifetime](docs/LIFETIME.md): SFX, bank and instance ownership rules.
- [Changes](CHANGELOG.md): release-specific compatibility notes.

The public headers are the API reference: `aicaflow/host.h` owns lifecycle and
instance control, while `aicaflow/bank.h` owns the AFB/AFX loader API.

## Requirements

- KallistiOS with `kos-cc`, `arm-eabi-gcc`, and `arm-eabi-objcopy`
- Python 3.10+ and `mido` (`python3 -m pip install mido`)
- A DKR checkout with its extracted assets

## Use with DKR

DKR pins this repository as `third_party/aicaflow`. A clone should use
submodules:

```sh
git clone --recurse-submodules git@github.com:dfchil/Diddy-Kong-Racing.git
cd Diddy-Kong-Racing
source ../enDJinn/environ.sh
python3 -m pip install mido
make -f Makefile.dc -j8 dkracing.elf
```

For an archive-only checkout, extract it as `third_party/aicaflow/` in the DKR
tree, then use the same command. `make -f Makefile.dc -j8 cdi` produces a CDI
and uses the `/cd` asset mount internally.

`MANIFEST.sha256` lists every shipped source file and its SHA-256 digest.
