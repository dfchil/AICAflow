# AICAflow specifications

[Documentation](../README.md)

File layouts, bytecode and the SH4/ARM7 interface:

- [Runtime ABI](runtime.md) — firmware bootstrap, commands and ownership.
- [Runtime asset formats](../../driver/format/docs/assets.md) — AFB, AFX, AFC and the
  optional SH4 AFI catalog.
- [AFX instruction language](../../driver/format/docs/instruction-language.md) — the exact timed
  bytecode in an AFX control stream.
- [SH-4 ↔ ARM7 IPC](ipc.md) — queue records, command ownership and durable
  observations.

Binary asset and IPC multibyte values are little-endian. The exact numeric
constants and C layouts are authoritative in
[`driver/format/include/aicaflow/format.h`](../../driver/format/include/aicaflow/format.h)
and the validator in `driver/format/src/codec.c`.
