# SH-4 ↔ ARM7 IPC

[Documentation](../README.md)

The fixed IPC area is the control plane between the SH-4 host library and the
ARM7 executor. Applications should use the public SH-4 API in
driver/sh4/include/aicaflow/host.h, not write the queue directly. This document
describes the wire contract used by that API.

The current firmware ABI is 7 and its fixed layout ID is defined in
driver/include/aicaflow/protocol.h. Bootstrap rejects a firmware image that
does not report matching values.

## Shared memory

| Region | Address | Owner | Purpose |
| --- | ---: | --- | --- |
| Status | 0x1fcec0 | ARM7 writes | ABI/layout, capabilities, timer, asset range, DSP result and stack margin |
| Command queue | 0x1ff7c0 | SH-4 publishes; ARM7 consumes | 32 fixed-size commands |
| Observed slots | 0x1fcf10 | ARM7 writes | durable state/result for up to 64 flow slots |
| Channel maps | 0x1fd710 | SH-4 prepares | five 64-byte local-to-physical channel-map arenas |

Each map entry is one byte: channel 0–63, or `0xff` for unused entries.
Map allocations start on four-byte boundaries and round up to whole words so
SH-4 uploads and clearing cannot overwrite neighbouring maps. The actual channel
count excludes padding. Allocations prefer the channel-count arena and spill
into other arenas when needed.

The full placement, including private ARM7 state and stacks, is in
[Memory layout](../memory.md). All fields are little-endian. The queue is
single producer/single consumer: SH-4 advances head after publishing a command;
ARM7 advances tail after consuming it.
The ring holds 32 commands. A full ring returns `-AFX_IPC_FULL` without
overwriting queued commands; callers may retry after ARM7 makes room. ARM7
consumes at most eight commands per main-loop pass. Authored AFX events do not
use this queue. ABI 7 requires rebuilding the host library and firmware together.

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
| ACTIVATE | afx_instance_activate | Start a validated AFX image with a channel map and bank base/size. |
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
afx_gain_payload_t, afx_tempo_payload_t, afx_lane_payload_t and afx_dsp_payload_t in protocol.h;
each is exactly 48 bytes.

DSP commands use flags=1. ENABLE supplies the allocated ring address and byte
size (both zero for no ring), followed by ten zero words. ARM7 validates the
2 KiB alignment, supported size and arena bounds. DISABLE requires a zero
payload and is safe to retry. SH4 owns the allocation until disable is
acknowledged; timeout alone never permits freeing it.

## Results and observations

The ARM7 publishes one afx_observed_t per flow slot. Its epoch is odd while
being updated and even when stable; SH-4 reads a matching stable epoch. It
contains the reference, state, command sequence, result, stream position, next
deadline and maximum lateness detail.

The states are FREE, RUNNING, PAUSED, PARKED, DONE and ERROR. An IPC command
can be accepted into the queue yet later produce an error, for example because
the instance became stale or a rebuild payload fails validation. DSP completion
is reported separately by dsp_sequence and dsp_result in the status block.

Activation encodes the bank base as a 16-bit address in 32-byte units and its
size as a 32-bit byte count. REBUILD sample address words are bank-relative,
not absolute AICA addresses. PATCH rejects CONTROL and SAMPLE_LOW. Firmware and host must be upgraded
together. File layouts are unchanged, but AFX streams that PATCH CONTROL or
SAMPLE_LOW are now rejected.
It never carries AFB/AFX file parsing, MIDI events, sample-name lookup, a seek
index or an authoring profile.
