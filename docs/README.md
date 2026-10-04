# AICAflow documentation

[Project overview](../README.md)

## Guides

- [Getting started](getting-started.md) — prerequisites, builds and playback.
- [Integration](integration.md) — load assets and control playback.
- [Lifetime](lifetime.md) — instances, banks and resource ownership.
- [Dependencies](dependencies.md) — setup and version updates.
- [Testing](testing.md) — host checks and hardware verification.
- [Release verification](release-check.md) — candidate checks and remaining work.

## Reference

- [DSP](dsp.md) — programming guide and instruction cheatsheet.
- [Memory layout](memory.md) — AICA RAM and reservations.
- [Runtime ABI](specs/runtime.md) — firmware bootstrap and execution.
- [SH4/ARM7 IPC](specs/ipc.md) — queues, commands and observations.
- [Runtime asset formats](../driver/format/docs/assets.md) — AFB, AFX, AFC and AFI.
- [AFX instruction language](../driver/format/docs/instruction-language.md) — control-stream bytecode.
- [Public format code](../driver/format/README.md) — portable headers and codec.
- C API: [playback](../driver/sh4/include/aicaflow/host.h),
  [banks](../driver/sh4/include/aicaflow/bank.h), [DSP](../driver/sh4/include/aicaflow/dsp.h).
- [Driver contract](../driver/CONTRACT.md) and [ARM7 executor](../driver/arm7/README.md).

## Examples and tools

- [Quickstart](../examples/quickstart/README.md) — minimal playback.
- [Multiple DSP effects](../examples/multiple_dsp_effects/README.md) — independent effect paths.
- [DSP effects player](../examples/dsp_effects_player/README.md) — preset audition.
- [Dynamic SFX](../examples/dynamic_sfx/README.md) — live sound controls.
- [Music player](../examples/music_player/README.md) — playlist, profiles and visualisation.
- [Player framework](../examples/player_framework/README.md) — shared example UI.
- [Runtime tools](../tools/README.md) — tuner and asset validator.
- [Tuner reference](tuner.md) and [server setup](../tools/tuner/server/README.md).

## Asset authoring

[AICAforge documentation](https://github.com/dfchil/AICAforge/blob/main/docs/README.md)
covers importers, bank building, profiles, authoring formats and asset tests.
See [asset licences](../ASSET_LICENSES.md) for distributed example inputs.
