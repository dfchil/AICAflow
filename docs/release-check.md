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
- PENDING: DSP preset player and dynamic SFX listening/controller tests.
- PENDING: tuner upload, replacement, seek, DSP, STOP/recycle and reset.

HDMI capture was not enumerated by AVFoundation; audio assessment uses the
user's listening confirmation, not a captured measurement.

Local logs and clean test checkouts: `/tmp/aica-release.jdm0NJ/`.
Do not publish final releases until the pending checks are resolved.
