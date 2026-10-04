# Public asset-format contract

This directory is the canonical portable format interface owned by AICAflow.
It is the future vendoring boundary for AICAforge, not a third repository.
Consumers add `format/include` to their include path and include
`<aicaflow/format.h>`. It requires no KOS headers or runtime implementation.

`AFX_FORMAT_API_VERSION` versions the source interface independently of
repository releases and the SH-4/ARM7 runtime ABI. Existing on-disk versions
remain unchanged: AFB 1, AFX 7, AFC 1, AFI 1 and checkpoint payload 1.
The `abi` member of `afx_file_header_t` contains the **AFX file version**,
not the firmware ABI.

The header documents layouts; callers must still decode little-endian bytes
instead of casting unaligned file data to C structs. The canonical descriptions
are [Assets and sidecars](../docs/specs/assets.md) and
[Instruction language](../docs/specs/instruction-language.md).

Run `make -C format check` (also included in the top-level `make check`).
The header is checked independently in C, C++ and assembly preprocessing.

## Migration status

This first step extracts definitions without changing format values or runtime
behavior. `driver/include/aicaflow/protocol.h` includes the public header for
existing runtime consumers. It retains firmware, IPC and memory-layout details.

Authoring is **not standalone yet**: portable codec extraction, checkpoint
types, target limits and independent build/test targets are subsequent steps.
Do not vendor the complete driver protocol into AICAforge to bypass that work.
The eventual vendored copy belongs at `dependencies/aicaflow-format/`, with
an explicit upstream revision and format API version.
