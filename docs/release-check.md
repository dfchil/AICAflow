# Release candidate verification

[Documentation](README.md)

Candidates: AICAflow 0.2.0-rc1 and AICAforge 0.1.0-rc1 (not tagged or published).
Checked 2026-10-04 from fresh origin clones of AICAflow `082b6e6` and
AICAforge `83774c3`, followed by candidate changes in AICAflow `7506997`
and AICAforge `3e7b377` (build fixes, gain calibration and release notes).

## Automated checks

- PASS: fresh AICAflow runtime tests before dependency initialization.
- PASS: pinned AICAforge authoring tests and AICAflow compatibility tests.
- PASS: standalone AICAforge tests and compatibility with its pinned SDK.
- PASS: all five Dreamcast examples and tuner build after the fix.
- PASS: music-player asset, filter and playback-control checks.
- PASS: rebuilt firmware matches its release SHA-256 manifest.
- PASS: regression test for missing sources under parallel Make.
- PASS: CI on the base commits:
  [AICAflow](https://github.com/dfchil/AICAflow/actions/runs/37232657664),
  [AICAforge](https://github.com/dfchil/AICAforge/actions/runs/37232647564).
- PASS: candidate CI including the build fix:
  [AICAflow 7506997](https://github.com/dfchil/AICAflow/actions/runs/37234484487),
  [AICAforge 3e7b377](https://github.com/dfchil/AICAforge/actions/runs/37234474270).

Build environment: macOS, KOS `c22f26c9`, enDjinn `a30476f`, firmware ABI 6.
Firmware SHA-256: `5328d3a2a7f24b68a78194579ed3181b3a81900c9f8b70d250f9503156bef571`.

## Hardware

The music maps now raise authored levels by approximately 4 dB. Base-asset
comparison confirms unchanged sample bytes, timing and other register fields;
Chopin expression levels and all profile bindings were updated. Its previous
profile hash did not match the fresh base build, so that build had skipped its
expression lanes. The user approved the +4 dB loudness balance on hardware;
this is listening feedback, not a measured clipping guarantee.

Dreamcast with BBA at `10.0.0.184`, loaded using kos-tool.

- PASS: quickstart returned `Aicaflow quickstart: PASS (0)`.
- PASS: user confirmed dual-DSP audio, stereo separation, replay and screen text.
- PASS: dual-DSP exit returned to the loader after the user ended the demo.
- PASS: music-player speed, 0–100% volume and +4 dB balance, confirmed by user.
- PASS: music-player song changes, seek and pause/stop, confirmed by user.
- PASS: music-player automated frame test on hardware (all three songs and AFV);
  corrected the test-header include path. Interactive executable rebuilt afterwards.
- PASS: isolated dry/wet captures of all 24 DSP presets and targeted-input tests
  of all 17 initially excluded presets. Initial exclusions are superseded by
  [retest measurements](../examples/dsp_effects_player/tests/hardware-retest.json).
- PASS: tuner bank/control uploads, DSP upload/readback, returns, STOP/recycle
  and reset during the audition runs.
- OPEN: one control upload returned `BAD_SAMPLE` after the pitch-shift audition;
  cause remains unconfirmed. Six pitch-shift/harmonizer pairs (12 plays) with
  one resident bank and no resets passed. A deliberately mismatched bank ID
  returned `BAD_SAMPLE`; restoring the matching flow recovered without reset.
- PASS: corrected doubled MADRS indexing in the encoder contract and all prefab
  callers. Hardware verifies Ping-pong, Multitap, Diffusion and Large room;
  regression tests cover direct MADRS indexing and six-bit MASA encoding.
- PASS: Ping-pong and Multitap demo levels retested without captured clipping;
  both and Large room restored to the menu.
- OPEN: two network/HDMI outages during extended tuner testing; recovered using
  `ensure_dctool_ready`. Cause is not established and is separate from BAD_SAMPLE.
- PASS: dynamic SFX engine/slide selection, controller response and graphics,
  confirmed by the user on 2026-10-05.
- PASS: dynamic SFX pan conversion and filter register packing corrected;
  regression checks cover all 32 pan positions, cutoff targets, Q and muting.
  Dreamcast build passes. A 20-second HDMI capture peaks at -13.38 dBFS with
  no full-scale samples; this is not an exhaustive clipping guarantee.
- PENDING: user confirmation of corrected dynamic SFX pan/filter behavior.
- PASS: tuner seek on 2026-10-05 with Bach AFB/AFX/AFC: forward/backward to
  0, 5, 10 and 30 seconds using one resident bank/flow, 1.5-second region stops
  and invalid-bound rejection. Fixed duration parsing: use `afx_flow_duration`
  instead of reserved header words. Host regression and Dreamcast build pass.
- PASS: user confirmed nine effects in the revised DSP menu sound good on
  hardware on 2026-10-05, including the Echo wet/dry comparison.
- RESOLVED: Bow texture removed from the demo menu after user listening found
  its effect unclear. The prefab remains available through the API; nine
  effects remain in the demo.
- PASS: DSP player exit returned `Program returned 0` after the listening test.
- PENDING: revised DSP player's explicit stop confirmation.

HDMI measurement uses native Live Gamer channels 0/1 at 48 kHz, without a
four-channel downmix. Measurements are not a guarantee of subjective audibility.

Local logs and clean test checkouts: `/tmp/aica-release.jdm0NJ/`.
Do not publish final releases until the pending checks are resolved.
