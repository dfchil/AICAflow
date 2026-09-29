# AICAflow DKR Edition 0.1.4

This is the minimal AICAflow source distribution needed by the Dreamcast port
of Diddy Kong Racing. It contains the AICA firmware, SH-4 host library, and
the N64 music/SFX import pipeline used by DKR.

It is released under the [MIT License](LICENSE).

It deliberately does **not** contain the editor, tuner, general MIDI tools,
other importers, examples, DSP labs, recordings, or unrelated documentation.

## Documentation

- [DKR integration](docs/INTEGRATION.md): runtime ownership and build paths.
- [N64 import](docs/N64_IMPORT.md): DKR's offline AFB/AFC asset pipeline.
- [Lifetime](docs/LIFETIME.md): SFX, bank and instance ownership rules.
- [Changes](CHANGELOG.md): release-specific compatibility notes.

The public headers are the API reference: `aicaflow/host.h` owns the runtime
API and `aicaflow/sfx_bank.h` owns the AFB1/AFC1 loader API.

## Requirements

- KallistiOS with `kos-cc` (no ARM7 toolchain required)
- Python 3.10+ and `mido` (`python3 -m pip install mido`)
- A DKR checkout with its extracted assets

The matching compiled firmware is checked in as `driver/arm7/aicaflow.drv`.
Normal builds and `make clean` preserve it. After changing ARM7 sources or the
shared protocol, rebuild it with the KOS ARM toolchain and commit the binary
alongside the sources:

```sh
source /opt/toolchains/dc/kos/environ.sh
make -C driver/arm7 clean
make -C driver/arm7 rebuild
```

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
