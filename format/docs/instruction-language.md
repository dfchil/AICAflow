# AFX instruction language

This is the bytecode inside a sample-free AFX image. It describes AFX file
version 7; that is distinct from the firmware SH-4/ARM7 ABI carried in the
status block.

The stream starts at **stream_offset** inside the AFX image. Its first
**setup_count × AFX_SETUP_BYTES** bytes are AICA register templates; optional
lane-map bytes, when AFX_FLAG_LANES is set, lie between those templates and the
stream. Every stream integer is little-endian. Instructions are byte-addressed
and need no individual alignment.

## Execution model

The ARM7 reads due instructions until it reaches a nonzero wait, completes or
parks the flow. A wait advances the flow's authored timeline; the scheduler
uses the shared AICA timer and keeps each instance's deadline independently.
The SH-4 assigns an instance's local channels to physical AICA channels before
activation. Thus the same flow can run concurrently without putting physical
channel numbers in the file.

The host validates the complete stream before upload. An event burst must fit
the recorded work profile, when present, and the runtime execution budget.
Malformed, zero-length waits, unknown opcodes, bad channels, invalid masks and
a missing or early terminal instruction are rejected.

## Opcodes

| Byte | Instruction | Wire fields after opcode | Bytes | Meaning |
| ---: | --- | --- | ---: | --- |
| 00 | END | — | 1 | Complete the finite flow. Must be the final instruction. |
| 01 | WAIT8 | ticks:u8 | 2 | Wait 1–255 authored ticks. |
| 02 | WAIT16 | ticks:u16 | 3 | Wait 1–65,535 authored ticks. |
| 03 | WAIT32 | ticks:u32 | 5 | Wait 1–4,294,967,295 authored ticks. The executor splits very long waits safely. |
| 10 | NOTE | channel:u8 setup:u16 mask:u32 values[] | variable | Copy one setup template, override selected fields, then key on. |
| 11 | PATCH | channel:u8 mask:u32 values[] | variable | Update selected fields of a running or parked local channel. |
| 12 | KEYOFF | channel:u8 | 2 | Release the mapped AICA voice. It does not itself end the flow. |
| 13 | PARK | — | 1 | Stop execution without releasing the controlled flow. Must be final and requires AFX_FLAG_CONTROLLED. |
| 14 | NOTE_PL | channel:u8 setup:u16 pitch:u16 mix:u16 | 8 | Compact NOTE form: only PITCH and MIX/TOTAL_LEVEL override the setup. |
| 15 | PATCH_LEVEL | channel:u8 mix:u16 | 4 | Compact PATCH form: only MIX/TOTAL_LEVEL changes. |

NOTE_PL and PATCH_LEVEL are compact encodings of NOTE and PATCH.

A finite one-shot normally emits NOTE, one or more WAITs, KEYOFF, any intended
release-tail WAIT, then END. A looping source sample does not make a flow
infinite: it still needs an authored KEYOFF and END. PARK is reserved for
SH-4-controlled flows.

## NOTE and PATCH field masks

A mask bit selects one 16-bit value. Values occur in ascending field-number
order. Therefore an instruction is base bytes plus 2 × popcount(mask) bytes.

| Bit | Field | AICA register word represented |
| ---: | --- | --- |
| 0 | CONTROL | playback format, high sample address and loop/key bits |
| 1 | SAMPLE_LOW | low sample-address word |
| 2–3 | LOOP_START, LOOP_END | loop points |
| 4–5 | ENV_AD, ENV_DR | amplitude envelope attack/decay/release words |
| 6 | PITCH | pitch register |
| 7 | LFO | LFO register |
| 8 | DSP_SEND | DSP routing/send word |
| 9 | DIRECT | direct path, pan and filter-Q word |
| 10 | MIX | total-level and LPF mixer word |
| 11–15 | FILTER_LEVEL0 … FILTER_LEVEL4 | filter envelope levels |
| 16–17 | FILTER_AD, FILTER_DR | filter envelope rates |

The accepted mask is exactly the low 18 bits represented above. A NOTE setup
index must be below setup_count; channels used by NOTE, PATCH and KEYOFF must
be below required_channels.

A NOTE copies its setup template and applies its masked values. A PATCH
updates the channel's running register state.

## Termination and timing constraints

- The stream contains exactly one terminal END or PARK, and it is last.
- WAIT values are nonzero.
- Only a controlled flow may end with PARK.
- A normal flow must use END; keying off a voice does not automatically retire
  the instance.
- The AFX header tick-rate numerator and denominator define authored ticks.
  Instance tempo scaling changes playback speed without changing the file.
- The compiler/host calculate burst work between waits and reject a declared
  work profile that disagrees with the stream.

Use `afx_encode_event()` and `afx_decode_event()` from
[`codec.h`](../include/aicaflow/codec.h) to encode and decode events.
