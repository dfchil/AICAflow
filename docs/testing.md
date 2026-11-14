# Testing

[Documentation](README.md)

`make check` (also `make runtime-check`) runs format, driver, DSP, tuner and
frozen-asset compatibility tests, including the firmware manifest hash.
It needs a C11 compiler with AddressSanitizer/UndefinedBehaviorSanitizer and
Python 3's standard library. Clang is the default; `make CC=gcc CXX=g++ check`
uses GCC. It needs no AICAforge checkout, Python packages, KOS or ROMs.

To exercise AICAforge output with AICAflow's validator and loader:

```sh
make authoring-dependencies
make compatibility-check
```

This checks current and frozen assets, bank binding and stale-checkpoint rejection.
Compiler tests, Python dependencies and generated-asset troubleshooting are
documented in [AICAforge testing](https://github.com/dfchil/AICAforge/blob/main/docs/testing.md).
`AICAFORGE_DIR` defaults to the parent of `AICAFORGE_BIN`; override it when
using a custom binary output directory. For an external checkout, pass `AICAFORGE_BIN=/absolute/AICAforge/build`.
CI checks both repositories together.

`make firmware-check` rebuilds the ARM7 firmware and compares its SHA-256
with `firmware/manifest.json`; this requires the ARM toolchain.

`make check` also builds the host backend (`driver/host`) and runs its
checks: the software AICA's voices and DSP, and the
console checks (`driver/tests/check_*.c`) unchanged against the firmware's executor
on the software AICA. `make -C driver/host web-check` runs the same checks
from an Emscripten build under Node; it needs `emcc` and `node`. To hear a
flow without a console:

```sh
make -C driver/host afx_render
driver/host/afx_render -s 30 bank.afb flow.afx out.wav
```

Host checks prove asset/command properties, not audible equality on hardware.
Hardware smoke testing is manual: run quickstart, the DSP demo and tuner,
verify playback, bank replacement, STOP/recycle, seek and a runtime DSP program.
Record source, KOS and firmware revisions with the result.
`make -C driver song-check` builds `driver/build/check_song.elf`, which prints
`AFX_SONG PASS` when a song with a continue join loops every 8160 ticks and one
with a new-play join every 7160 (the next play starting while the last rings
out), measured from activation, a song restarts while its last instance stops,
a paused song resumes at the position its status reports, and every runtime
block is given back. The checks' fixtures come from
`driver/tests/make_fixtures.py`.
`make -C driver commands-check` builds `driver/build/check_commands.elf`,
which checks the command semantics on the console and prints `AFX_CMD PASS`:
STOP right behind ACTIVATE, channels back at STOP with all 64 in use,
`afx_instance_replace()`, `afx_asset_release()`, and a PATCH sent before a
channel's first NOTE, read back from the channel's DIRECT register.
`make -C driver banks-check` builds `driver/build/check_banks.elf`, which
prints `AFX_BANKS PASS` when a flow whose setup is in bank slot 1 sounds from
that bank (its sample address read back beside a one-bank instance) and both
banks are freed with the last flow that plays from them.
`make -C driver scenes-check` builds `driver/build/check_scenes.elf`, which
prints `AFX_SCENES PASS` when, beside a permanent scene that holds the core
bank and the DSP ring, a level scene and its song slot are loaded, played and
released eight times with three songs each. The level's flow binds the core
bank and sounds from the level's own bank, each song sounds from the slot, a
slot whose song plays keeps it, and every release gives the arena's largest
free block back. The classical
player's `make frame-test` exercises its real loader and exits after all three
pieces; its [README](../examples/music_player/README.md#hardware-frame-test)
also explains how to restore an interactive build afterwards.

Game integration checks require the application's own extracted inputs; the
generic tests do not distribute or extract game assets.
