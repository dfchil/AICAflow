# Changelog

## 0.1.8-dkr

- Moved the DKR music player into AICAflow as a self-contained example with
  its checked-in shared AFB, compact AFX flows, AFC seek data and VIZ files.

## 0.1.7-dkr

- The generic music player ignores player controls held while it starts, until
  they have been released once. enDjinn's `START+A+B+X+Y` soft reset remains
  enabled.

## 0.1.6-dkr

- Compact shared music banks again use PCM8 by default and retain ADPCM only
  when the established full-sample and attack-quality thresholds both pass.

## 0.1.5-dkr

- Switched the DKR package to the final AFB sample-bank, sample-free AFX flow,
  and optional SH-4-only AFC seek-index model.
- Removed the AFB1/AFC1 loaders and embedded-sample AFX compatibility paths.
- Kept DKR's room DSP as a runtime C program.

## 0.1.4-dkr

- Added DKR-only integration, N64 import and lifetime documentation.
- Documented the pinned-submodule installation model.

## 0.1.3-dkr

- Corrected the public DKR package README and ignored Python bytecode.
- N64 SFX import distinguishes sample loops from envelope sustain, so finite
  pickup sounds emit `KEYOFF`/`END` instead of parking indefinitely.
