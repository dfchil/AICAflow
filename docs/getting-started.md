# Getting started

AICAflow runs prepared AFB/AFX assets on Dreamcast AICA hardware.  The package
includes a verified ARM7 firmware image, so normal example builds need only a
KallistiOS SH4 environment and host utilities (Clang with C11 support, zlib,
`curl`, `unzip` and a SHA-256 command). The example C sources use C23 `#embed`,
so use a KOS compiler that supports it. macOS and Linux are the supported host
workflows. The example toolchain is a pinned AICAforge submodule:

```sh
git clone https://github.com/dfchil/aicaflow.git
cd aicaflow
make dependencies
source /opt/toolchains/dc/kos/environ.sh
make examples
```

`firmware/aicaflow.drv` is embedded by every example.  It is validated at
runtime and does not require an ARM7 compiler.  Maintainers with the full KOS
toolchain can regenerate and verify it with `make firmware-check`.

`examples/music_player` fetches its pinned score inputs, GeneralUser GS and
Cello Solo SoundFonts when first built. Its [README](../examples/music_player/README.md)
describes the exact sources, licenses and `PIANO_SOUNDFONT`, `CELLO_SOUNDFONT`
and `ORCHESTRA_SOUNDFONT` overrides. Each piece has its own bank; selecting a
new piece releases the previous one.

To work only on offline assets, use AICAforge; it needs neither KOS nor
an ARM7 toolchain. To use an existing authoring build, pass
`AICAFORGE_BIN=/absolute/AICAforge/build` to the example make invocation.
To run the resident BBA tuner, run `make tools` and follow [Tuner](tuner.md).
`make check` tests the runtime without authoring dependencies; see [Testing](testing.md).

Interactive examples and the tuner use `dependencies/enDjinn`; the driver does not.
After moving a checkout or its dependencies, clean the example and tuner build
directories to remove generated `.d` files containing old absolute paths.
