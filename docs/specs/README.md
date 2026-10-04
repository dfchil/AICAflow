# AICAflow specifications

File layouts, bytecode and the SH4/ARM7 interface:

- [Runtime ABI](runtime.md) — firmware bootstrap, commands and ownership.
- [Assets and sidecars](assets.md) — AFB, AFX, AFC, AFV, AFP, AFBM and the
  optional SH4 AFI catalog.
- [SFX bank maps](https://github.com/dfchil/AICAforge/blob/main/docs/specs/afsfx.md) — offline SFX grouping and DKR map grammar.
- [AFX instruction language](instruction-language.md) — the exact timed
  bytecode in an AFX control stream.
- [SH-4 ↔ ARM7 IPC](ipc.md) — queue records, command ownership and durable
  observations.

Binary asset and IPC multibyte values are little-endian. AFP is JSON, and
AFBM/AFSFX are text; they are not uploaded to AICA. The exact numeric
constants and C layouts are authoritative in
[`format/include/aicaflow/format.h`](../../format/include/aicaflow/format.h)
and the validator in `format/src/codec.c`.
