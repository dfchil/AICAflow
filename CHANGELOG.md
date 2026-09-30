# Changelog

## Unreleased

- Added a self-contained `examples/dsp_demo` that authors the ping-pong DSP
  program at runtime through the public C API.
- Made the MIDI compiler defer SoundFont loading until an SF2 mapping is used,
  so the sine-only demo does not require that optional path.

## 0.1.4-dkr

- Added DKR-only integration, N64 import and lifetime documentation.
- Documented the pinned-submodule installation model.

## 0.1.3-dkr

- Corrected the public DKR package README and ignored Python bytecode.
- N64 SFX import distinguishes sample loops from envelope sustain, so finite
  pickup sounds emit `KEYOFF`/`END` instead of parking indefinitely.
