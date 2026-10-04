# Changelog

## 0.2.0-rc1 (unreleased)

- Bank-bound AFB/AFX playback with shared sample banks and setup templates.
- SH4 memory management, seeking, live controls and programmable DSP scenes.
- ARM7 firmware included for builds without an ARM7 toolchain.
- Pinned AICAforge toolchain for example asset generation.
- Quickstart, DSP, dynamic-SFX and classical music examples; persistent BBA tuner.
- Correct first-build download dependencies for parallel music-player builds.
- Correct the shared player's frame-test header path.
- Approximately +4 dB authored music levels with matching performance profiles.

### Migration from the combined repository

- Run `make dependencies` before building examples. Do not initialize submodules recursively.
- Replace `make compiler` with `make authoring`, or build AICAforge separately.
- Replace `make authoring-check` with `make -C dependencies/AICAforge check`.
- `make check` tests the runtime; `make compatibility-check` also tests tool output.
- Default authoring executables are in `dependencies/AICAforge/build/`, not `build/`.
  Set `AICAFORGE_BIN` to use an external build.
- Change custom include/source paths from `format/` to `driver/format/`.
- The unused `afx_dsp_program` exporter is removed. Construct DSP programs with
  `<aicaflow/dsp.h>` and install them with `afx_dsp_scene_program()`.

Since `repo-split-v1`, runtime C API signatures, firmware IPC ABI 6 and file
versions AFB 1 / AFX 7 / AFC 1 / AFI 1 are unchanged. Existing runtime assets
remain compatible. Build commands and repository paths are breaking changes.

See [release verification](docs/release-check.md) for candidate test status.
