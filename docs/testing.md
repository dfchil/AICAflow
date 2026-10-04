# Testing

`make check` (also `make runtime-check`) runs format, driver, DSP, tuner and
frozen-asset compatibility tests, including the firmware manifest hash.
It needs Clang with AddressSanitizer/UndefinedBehaviorSanitizer and Python 3's
standard library. It needs no AICAforge checkout, Python packages, KOS or ROMs.

Authoring tests belong to AICAforge. To additionally exercise its output with
AICAflow's validator and loader:

```sh
make -C /path/to/AICAforge check
make compatibility-check AICAFORGE_BIN=/path/to/AICAforge/build
```

The integration target reuses AICAforge's CLI tests for bank merging, profiles
and deterministic output; AICAflow supplies the validator. It also validates
current and frozen older assets, bank binding and stale-checkpoint rejection.
`AICAFORGE_DIR` defaults to the parent of `AICAFORGE_BIN`; override it when
using a custom binary output directory. AICAforge's tests need its documented
Python dependencies. CI checks both repositories together.

`make firmware-check` rebuilds the ARM7 firmware and compares its SHA-256
with `firmware/manifest.json`; this requires the ARM toolchain.

Host checks prove asset/command properties, not audible equality on hardware.
Hardware smoke testing is manual: run quickstart, the DSP demo and tuner,
verify playback, bank replacement, STOP/recycle, seek and a runtime DSP program.
Record source, KOS and firmware revisions with the result. The classical
player's `make frame-test` exercises its real loader and exits after all three
pieces; its [README](../examples/music_player/README.md#hardware-frame-test)
also explains how to restore an interactive build afterwards.

Game integration checks require the application's own extracted inputs; the
generic tests do not distribute or extract game assets.
