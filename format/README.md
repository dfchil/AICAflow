# Public asset-format contract

This is the canonical portable contract owned by AICAflow. AICAforge uses
this directory directly from its pinned `dependencies/AICAflow/` driver SDK;
there is no separate format copy. The portable code has no KOS or enDjinn
dependencies. Add `include/` to the include path and compile
`src/codec.c` when decoding or validating assets.

- `format.h`: file layouts, field IDs, commands and checkpoint records.
- `codec.h` / `src/codec.c`: little-endian access, event codec and validation.
- `limits.h`: target acceptance limits, not firmware addresses.
- `result.h`: stable result numbers shared with runtime callers.

`AFX_FORMAT_API_VERSION=1` versions this source interface independently of
repository releases. On-disk versions remain AFB 1, AFX 7, AFC 1, AFI 1 and
checkpoint payload 1. The AFX header's `abi` field is the **file version**;
firmware ABI 6 and IPC/memory layout stay private to the runtime protocol.

Read [Assets and sidecars](docs/assets.md) and
[Instruction language](docs/instruction-language.md). Decode little-endian
bytes; do not cast unaligned file data to C structs.

Run `make check` here for independent C/C++/assembly-header checks.
Native authoring and runtime suites exercise the codec separately. Firmware
validation lives in `driver/common/firmware.c`, outside this dependency.

Consumers pin the driver revision with a Git submodule; maintainers update it
explicitly and run compatibility tests before publishing. A repository version
change does not itself change the binary format version.
