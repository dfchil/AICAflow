# AICAflow runtime contract

[Documentation](../docs/README.md)

See [Runtime ABI](../docs/specs/runtime.md) for ownership and execution,
and [Runtime asset formats](format/docs/assets.md) for file layouts.

The headers define the numeric contract:

- [`include/aicaflow/protocol.h`](include/aicaflow/protocol.h) — ARM7/SH-4 ABI,
  control region and IPC constants.
- [`format/include/aicaflow/format.h`](format/include/aicaflow/format.h) —
  runtime file layouts and command encoding.
- [`sh4/include/aicaflow/bank.h`](sh4/include/aicaflow/bank.h) — AFB/AFX loader
  and bank lifetime API.
