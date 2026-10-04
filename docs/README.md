# AICAflow documentation

See [Repository split](repository-split.md) for AICAflow/AICAforge ownership,
the format boundary, test targets and the deprecation window.

AICAflow separates offline sound authoring from Dreamcast playback.
A control flow contains timed AICA register operations; its samples
live in a separately loaded bank. The SH-4 owns memory, validation, instances,
seeking, live controls and DSP scenes. The ARM7 firmware only runs bounded,
already-resolved commands.

## Read by task

- [Getting started](getting-started.md) — build and run the smallest example.
- [Integration](integration.md) and [lifetime](lifetime.md) — embed the driver
  safely in a Dreamcast program.
- [Authoring](authoring.md) — create a bank, flows and sidecars from MIDI,
  PCM, SoundFonts, N64 CSeq or MultiPCM captures; distinguish bank policy from
  performance profiles.
- [SFX bank maps](specs/afsfx.md) — `.afsfx` grouping/residency, raw N64 IDs,
  the implemented DKR grammar and its application-specific limits.
- [DSP](dsp.md) — construct and install an AICA DSP program.
- [Tuner](tuner.md) — use the persistent hardware development server.
- [Memory layout](memory.md) — ownership and the AICA RAM arena.
- [Specifications](specs/README.md) — assets, bytecode and SH-4/ARM7 wire ABI.
- [Testing](testing.md) — host and firmware checks.

The source headers define the numeric contract:
[`protocol.h`](../driver/include/aicaflow/protocol.h) defines the ARM7/SH-4
wire ABI and [`bank.h`](../driver/sh4/include/aicaflow/bank.h) defines the
bank loader API.
