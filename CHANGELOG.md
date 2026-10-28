# Changelog

## Unreleased

Changes since 0.2.0-rc1. The entries up to 2026-10-09 were reconstructed from
the commit history.

### Added

- Looping flows. A stream may end with `LOOP target` instead of `END`
  (`AFX_OP_LOOP`, flag `AFX_FLAG_LOOP`, header field `loop_tick`): the ARM7
  continues at the target in the same tick, so a song with an intro repeats
  without a gap or drift and without the SH4's help. Validation requires the
  target to be an instruction boundary with a WAIT between it and the LOOP,
  and `loop_tick` to be the target's authored tick. Each loop is reported to
  the host: the observed state word carries the instance's loop count above
  the state (firmware ABI 9), `afx_instance_status_t` has `loops`, and
  `afx_set_loop_callback()` is called per loop, before the end callback, with
  the loop target as position (image-relative) and the hardware tick the flow
  continued at as `next_deadline`. Checked on hardware with
  `make -C driver loop-check` (tests/check_loop.c): four loops of the
  quickstart fixture, 8160 ticks apart every time. `afx_asset_loops()` tells whether a flow loops and at which
  authored tick. A looping instance never ends by itself. The firmware and the
  host library must be updated together; the AFX file version is unchanged
  (older validators reject the flag and the opcode). The authoring tools do
  not emit LOOP yet.
- Compact envelope patches: `PATCH_ENV` (0x17, channel and ENV_AD, 4 bytes)
  and `PATCH_ENV_LEVEL` (0x18, ENV_AD and MIX, 6 bytes) decode to PATCH like
  `PATCH_LEVEL`. A flow can shape a note's fade with the AICA envelope, whose
  decay rate applies at once when changed mid-note, instead of a run of level
  steps. `afx_compact_opcode()` picks the compact form for an opcode and mask.
  Format API 3; firmware ABI 10 (ABI 9 firmware rejects the opcodes).

### Changed

- Firmware ABI 8: the ARM7 reports back to the SH4 by interrupt, and the
  program no longer polls. When an instance's state, sequence or result
  changes, the firmware puts its slot in an event ring in the queue header and
  raises the AICA's main-CPU interrupt. AICAflow's event thread (a KOS
  threaded handler on `ASIC_EVT_SPU_IRQ`) reads only the slots reported,
  updates `afx_instance_status()`, recycles an instance that ended and reports
  it to the end callback (`afx_set_end_callback()`). `afx_update()` and
  `afx_instance_recycle()` are gone. `afx_init()` fails with `AFX_TIMEOUT` if
  the firmware's boot interrupt does not arrive. A RUNNING instance's position
  in `afx_instance_status()` is as of its last change; `afx_instance_refresh()`
  reads it now. The host library and `firmware/aicaflow.drv` must be updated
  together.
- Firmware ABI 7 with a new layout ID: bank-relative playback, compact
  per-instance state and DSP delay rings allocated from the asset arena. The
  host library and `firmware/aicaflow.drv` must be updated together.
- The SH4 keeps no copy of an uploaded AFX image.
- The SH4 library defaults to trusted assets (`AFX_VALIDATE_ASSETS=0`): it
  checks layout, relocations and bank binding but does not decode the stream.
  Assets must carry an authored work profile. Build with
  `AFX_VALIDATE_ASSETS=1` for full validation.
- The runtime no longer refuses work. `afx_instance_activate()` and
  `afx_bank_flow_upload()` do not return `AFX_NO_EXEC_BUDGET`; the ARM7 runs
  every due event and reports lateness. `AFX_EXECUTION_BUDGET_*` remain the
  limits authoring tools apply per flow.
- `afx_init()` clears only the control region of sound RAM, not all 2 MB.
- A flow image uploads in one call, and the command queue head is kept on the
  SH4 instead of being read back.
- Queuing a command reads the queue tail back over G2 only when the last tail
  read shows no room, so at most once per 32 commands instead of once per
  command. The firmware and ABI are unchanged.
- When firmware advertises `AFX_CAP_DSP_HOST_INIT`, the SH4 fills a DSP ring
  in blocks. Aligned sample data is uploaded by DMA directly.
- Fewer status reads per `afx_update()`.
- Host checks build with GCC as well as Clang (`make CC=gcc CXX=g++ check`).
- `make check` validates the checked-in firmware image against the headers.
- The enDjinn submodule uses an HTTPS URL, so `make dependencies` needs no
  SSH key.
- The pinned AICAforge moves work beyond one tick's limit into the next tick
  instead of failing the flow.
- The pinned AICAforge has its review fixes and `afx_n64 --sfx-adpcm`.

### Fixed

- Firmware: a gain change kept only four bits of the mix word's low byte, so
  it cleared the low-pass bypass and voice-off flags and cut the filter Q.
  Only the attenuation byte is scaled now.
- Firmware: a PAUSE that arrived just after a flow ended by itself overwrote
  its DONE record with an error. It is answered like STOP now.
- A stream upload chunk that is not a multiple of 32 bytes is rejected unless
  it is the bank's last. It used to be accepted and then fail as `AFX_BUSY`.
- `afx_dsp_program_pingpong()` rejects a delay of 32768 samples or more, which
  read the other line's half of the ring.
- A handle to an asset still being uploaded goes stale across
  `afx_shutdown()` and `afx_init()`.
- Changing `AFX_VALIDATE_ASSETS` rebuilds the library without `make clean`.
- The example player frees a bank whose upload it cancels while DMA is in
  flight, instead of leaking it and blocking later loads.
- Bank upload and the tuner's TCP transfers use bounded memory.
- Checkpoint generation is deterministic.

### Removed

- `AFX_UPLOAD_STEP_BYTES`, and the declaration of `afx_status()`, which was
  never defined.

### Known limitations

- Bank loading is not thread-safe; see [Integration](docs/integration.md).

## 0.2.0-rc1 — 2026-10-05

- Bank-bound AFB/AFX playback with shared sample banks and setup templates.
- SH4 memory management, seeking, live controls and programmable DSP scenes.
- ARM7 firmware included for builds without an ARM7 toolchain.
- Pinned AICAforge toolchain for example asset generation.
- Quickstart, DSP, dynamic-SFX and classical music examples; persistent BBA tuner.
- Correct first-build download dependencies for parallel music-player builds.
- Correct the shared player's frame-test header path.
- Approximately +4 dB authored music levels with matching performance profiles.
- Correct MADRS register indexing in DSP delay, multitap and room prefabs.
- Calculate tuner playback duration from AFX instructions so region seeking works.
- Correct dynamic-SFX pan encoding, filter resonance and live cutoff control.
- Nine hardware-auditioned DSP examples using one resident sample bank.

### Known issues

- Extended tuner testing encountered one unreproduced `BAD_SAMPLE` upload and
  two network/HDMI outages. Causes remain unconfirmed; see the release checks.
- DSP prefabs outside the listener's nine selected effects are experimental;
  pitch-shift and harmonizer musical ratios are not calibrated.

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
- Custom DSP programs must use `afx_dsp_program_address(program, masa, offset)`
  directly; do not double `masa`. All 64 MADRS registers are addressable.

Since `repo-split-v1`, runtime C API signatures, firmware IPC ABI 6 and file
versions AFB 1 / AFX 7 / AFC 1 / AFI 1 are unchanged. Existing runtime assets
remain compatible. Build commands and repository paths are breaking changes.

See [release verification](docs/release-check.md) for candidate test status.
