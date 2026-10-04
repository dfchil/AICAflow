# Runtime ABI

[Documentation](../README.md)

## Responsibilities

The SH-4 owns the AICA RAM arena, firmware bootstrap, AFB validation and
upload, AFX validation and relocation, asset lifetime, instance handles,
seeking, live register updates and DSP scene installation. The ARM7 owns the
bounded scheduler and writes AICA channel/DSP registers. It never parses a
file, allocates sample memory or looks up a sample by name or index.

The firmware reports its ABI, layout ID, asset base and active asset limit in
the fixed status block. The host rejects a firmware image whose ABI or layout
does not match its headers. See [the memory map](../memory.md).

## Control flow

An AFX image has a fixed 80-byte header followed by relocation records and an
aligned image. The image begins with `setup_count` AICA register templates
(`AFX_SETUP_BYTES` each), then a timed bytecode stream. Its header binds it to
one 64-bit AFB identity; identity zero is allowed only for a flow with no
sample relocation.

The relevant stream operations are `WAIT8`, `WAIT16`, `WAIT32`, `NOTE`,
`NOTE_PL`, `PATCH`, `PATCH_LEVEL`, `KEYOFF`, `PARK` and `END`. A finite sound
must explicitly reach `KEYOFF` then `END`, even if its source sample loops.
`PARK` is only for a controlled flow that the SH-4 will resume or stop.

[AFX instruction language](../../driver/format/docs/instruction-language.md) specifies every opcode,
field mask, byte layout and stream validation rule. [SH-4 ↔ ARM7 IPC](ipc.md)
specifies the separate control queue; it is not part of an AFX file.

At upload, the SH-4 validates the AFX and every relocation against the bound
AFB payload, turns bank-relative addresses into AICA addresses once, and
submits the resolved image. No per-note lookup happens on ARM7.

## Live control and DSP

The command queue carries activation, stop/pause/rebuild/recycle, register
patches, per-instance gain and tempo, lane values, and DSP enable/disable.
Register fields cover the ordinary AICA channel controls: sample format and
address, loop points, envelope, pitch, LFO, direct path, filter, mixer level
and DSP send.

SH4 maintains seek reconstruction and lane state. A DSP scene may
reserve an external delay ring; its selected size changes the asset ceiling as
described in [Memory layout](../memory.md).

## Lifetime rule

Load an AFB, then upload each AFX bound to it. A flow retains its bank, so a
bank release fails while a dependent flow exists. Stop/free flows before
releasing their bank. More operational detail is in
[lifetime](../lifetime.md) and the public C API comments in
[`host.h`](../../driver/sh4/include/aicaflow/host.h).
