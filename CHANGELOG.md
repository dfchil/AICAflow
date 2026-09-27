# Changelog

## 0.1.4-dkr

- Added DKR-only integration, N64 import and lifetime documentation.
- Documented the pinned-submodule installation model.

## 0.1.3-dkr

- Corrected the public DKR package README and ignored Python bytecode.
- N64 SFX import distinguishes sample loops from envelope sustain, so finite
  pickup sounds emit `KEYOFF`/`END` instead of parking indefinitely.
