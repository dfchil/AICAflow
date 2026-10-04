# Public asset-format contract

Portable runtime format definitions, decoding and validation, used by AICAflow
and AICAforge. No KOS or enDjinn dependency. Add `include/` to the include path
and compile `src/codec.c`.

- `format.h`: file layouts, field IDs, commands and checkpoint records.
- `codec.h` / `src/codec.c`: little-endian access, event codec and validation.
- `limits.h`: target acceptance limits, not firmware addresses.
- `result.h`: stable result numbers shared with runtime callers.

`AFX_FORMAT_API_VERSION=1` versions this source interface independently of
repository releases. On-disk versions remain AFB 1, AFX 7, AFC 1, AFI 1 and
checkpoint payload 1. The AFX header's `abi` field is the **file version**;
firmware ABI 6 and IPC/memory layout stay private to the runtime protocol.

Read [Runtime asset formats](docs/assets.md) and
[Instruction language](docs/instruction-language.md). Decode little-endian
bytes; do not cast unaligned file data to C structs.

Run `make check` here for independent C/C++/assembly-header checks.
Firmware validation lives in `driver/common/firmware.c`.
