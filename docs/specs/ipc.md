# SH-4 ↔ ARM7 IPC

The fixed IPC area is the control plane between the SH-4 host library and the
ARM7 executor. Applications should use the public SH-4 API in
driver/sh4/include/aicaflow/host.h, not write the queue directly. This document
describes the wire contract used by that API.

The current firmware ABI is 6 and its fixed layout ID is defined in
driver/include/aicaflow/protocol.h. Bootstrap rejects a firmware image that
does not report matching values.

## Shared memory

| Region | Address | Owner | Purpose |
| --- | ---: | --- | --- |
| Status | 0x1fc000 | ARM7 writes | ABI/layout, capabilities, timer, asset range, DSP result and stack margin |
| Command queue | 0x1fc100 | SH-4 publishes; ARM7 consumes | 64 fixed-size commands |
| Observed slots | 0x1fd200 | ARM7 writes | durable state/result for up to 64 flow slots |
| Channel maps | 0x1fda00 | SH-4 prepares | five 64-entry local-to-physical channel maps |

The full placement, including private ARM7 state and stacks, is in
[Memory layout](../memory.md). All fields are little-endian. The queue is
single producer/single consumer: SH-4 advances head after publishing a command;
ARM7 advances tail after consuming it.

## Command record

Every record is 64 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | opcode |
| 4 | 4 | caller sequence |
| 8 | 4 | instance/flow reference |
| 12 | 4 | command flags |
| 16 | 48 | opcode-specific payload |

Queueing means only that the SH-4 published the request. Call afx_update() and
inspect afx_instance_status() for ARM7-owned durable completion. Handle
generation prevents stale instance references from targeting a recycled slot.

## Commands

| Opcode | Public API | Meaning |
| --- | --- | --- |
| ACTIVATE | afx_instance_activate | Start a validated, resolved AFX image with a channel map. |
| STOP | afx_instance_stop | Stop and release its mapped voices. |
| PAUSE | afx_instance_pause | Freeze its timeline without discarding flow state. |
| REBUILD | afx_instance_rebuild, afx_instance_seek | Restore SH-4-reconstructed channel state and resume or seek. |
| RECYCLE | afx_instance_recycle | Retire a completed or stopped slot for reuse. |
| PATCH | afx_instance_patch | Write selected complete AICA register words for one local channel. |
| INSTANCE_GAIN | afx_instance_gain | Apply linear instance gain, 0–255. |
| INSTANCE_TEMPO | afx_instance_tempo | Set one whole-flow Q8.8 tempo scale. |
| LANE_SET | afx_instance_lanes_set | Update persistent lane gain/mute/pan/DSP-send modifiers. |
| DSP_ENABLE | afx_dsp_scene_program or afx_dsp_scene_program_ring | Install the one scene DSP program and optional delay ring. |
| DSP_DISABLE | afx_dsp_scene_disable | Disable the scene and return its reserved ring. |

NOP is reserved. The command payload layouts are the fixed C types
afx_activation_t, afx_patch_payload_t, afx_rebuild_payload_t,
afx_gain_payload_t, afx_tempo_payload_t and afx_lane_payload_t in protocol.h;
each is exactly 48 bytes.

## Results and observations

The ARM7 publishes one afx_observed_t per flow slot. Its epoch is odd while
being updated and even when stable; SH-4 reads a matching stable epoch. It
contains the reference, state, command sequence, result, stream position, next
deadline and maximum lateness detail.

The states are FREE, RUNNING, PAUSED, PARKED, DONE and ERROR. An IPC command
can be accepted into the queue yet later produce an error, for example because
the instance became stale or a rebuild payload fails validation. DSP completion
is reported separately by dsp_sequence and dsp_result in the status block.

IPC contains only resolved AICA addresses, channel mappings and control values.
It never carries AFB/AFX file parsing, MIDI events, sample-name lookup, a seek
index or an authoring profile.
