# Design: songs made of sections

Status: decided 2026-11-14, being built (firmware ABI 14, format API 6).
Daniel decided the direction (relayed by daytona-arcade-recomp-d2) and
answered the open questions of the first draft; SM64 (sm64-dc-f9) and Daytona
reviewed it. `REPEAT` (AICAflow #24) is dropped, and so is `LOOP`: sections
with a "continue" join replace it.

## Why sections

A song such as Daytona's t03 is intro, A, A, B, then back to the first A.
Daytona's arcade board allocates voices dynamically, so the second A cannot
reuse the first A's channels at the join: the first A's last notes are still
ringing when the second A starts. Such a join starts the next section on a
second executor with fresh voices while the old play rings out. SM64's looping
songs and continuous sounds (engines, wind) need the opposite: a voice that
crosses the loop without a new key-on. That join continues on the same
executor and channels, as `LOOP` did. A song may use both kinds.

## Instructions

- `NEXT mode` (0x19, 2 bytes) ends a section's body. In the tick it runs, the
  song moves to the next order position:
  - `AFX_JOIN_NEW` (0): the next section starts on the song's other executor,
    in the same tick, with voices of its own; this executor goes on with its
    tail until `END`.
  - `AFX_JOIN_CONTINUE` (1): this executor jumps to the start of the next
    section in the same tick and keeps its voices, as `LOOP` did. Only `END`
    may follow it: there is no tail.

  At the end of the order the song goes on at its loop index, and that wrap
  is reported as a loop. A song without a loop index plays no further
  section; the `NEXT` then only marks the body's end.
- `FREE channel` (0x1a, 2 bytes) gives the voice of a local channel back to
  the song's pool, keying it off if it still sounds. The importer puts it
  after the channel's last release. In a song without a pool it does nothing.
- `LOOP` (0x16) and `AFX_FLAG_LOOP` are gone; an AFX header's former
  `loop_tick` word must be zero. Plain flows end with `END` or `PARK`.

A section's events after `NEXT` are its tail: its own voices' key-offs, level
patches and releases, which run past the start of the next section.

## The song file (AFM)

One AFM per song, next to its AFB. It holds what a flow holds, once for the
whole song: one setup table and its relocations, the bank identities (with
`AFX_FLAG_BANKS`, slots 1 to 3 as in a flow), the lane map. Then each
section's stream, and the order with its loop index.

Header (96 bytes, little-endian words):

| Offset | Field |
|---|---|
| 0 | magic `AFM1` (0x314d4641) |
| 4 | version 1 |
| 8 | total size |
| 12 | flags: `AFX_FLAG_MUSIC`, and `LANES`, `BANKS` as in a flow |
| 16, 20 | image offset (32-aligned), image size |
| 24 | setup count |
| 28 | relocations offset (count = setup count, 12 bytes each, as in AFX) |
| 32 | banks offset (as in AFX), or 0 |
| 36, 40 | bank identity, low and high |
| 44 | channels: local channels used, the largest of all sections |
| 48 | voices: the pool a song with a new-play join needs, else 0 |
| 52, 56 | tick rate numerator and denominator |
| 60 | work profile |
| 64 | control id (hash of the image) |
| 68..95 | zero |

The image is uploaded unchanged (bank table filled in by the loader):

| Image offset | Contents |
|---|---|
| 0 | image header (32 bytes): section count, order count, loop index (0xffffffff for none), section table offset, order offset, streams offset, streams size, 0 |
| 32 | setups (setup count × 40 bytes) |
| … | lane map (`channels` bytes, with `LANES`), padded to 4 |
| … | section table: per section its stream offset, stream size and body ticks (12 bytes) |
| … | order: section indices, 16 bits each, padded to 4 |
| … | streams |
| end | bank table (with `BANKS`), zero in the file |

Setups at 32 and the lane map right after them are where a flow keeps them, so
the firmware's activation finds them the same way; the bank table follows the
streams, so `AFX_BANK_TABLE_OFFSET(streams end)` finds it as in a flow.

## Two kinds of song at run time

- **Continue-only songs** (no `AFX_JOIN_NEW`): one executor, the instance's
  runtime slot, exactly like a flow. A `NEXT` continue is a jump; voices are
  the instance's channels, local channel i on map entry i. Any number of
  instances of one song may play at once, as with a flow. SM64's looping
  songs and its 114 continuous sounds are of this kind.
- **Songs with a pool** (at least one `AFX_JOIN_NEW`): at activation the SH-4
  grants the pool (`voices` channels, through the instance's channel map) and
  allocates a 288-byte runtime block for this instance: a control word block
  and two executors, each a runtime slot and a 64-entry local-to-physical map.
  The ARM7 maps a local channel onto a free pool voice at its first event in a
  play and gives it back at `FREE` or `END`; a continued executor keeps its
  mapping. The SH-4 frees the block when it recycles the instance. Restarting
  a song while its previous instance still stops therefore works for both
  kinds.

The instance's own runtime slot holds what applies to the whole song: handle,
state, gain, tempo, lane count, loop count, lateness. Gain, tempo, lanes,
pause and stop act on both executors. The firmware's private memory grows by
one 16-bit order position per instance slot (128 bytes).

## Positions, pause and resume

`afx_instance_status` on a song reports `state` and `next_deadline` as for a
flow. `position` packs the order position of the newest section (high 12
bits) and that executor's offset in its section's stream (low 20 bits).
`afx_instance_rebuild` on a song takes that value back and resumes the newest
section there, with the remaining wait carried over; a tail that was ringing
at the pause is not resumed. `afx_song_seek(instance, position)` restarts at
the start of an order position. Seeking inside a section is a later step.

## Load-time checks and the compiler

`afx_song_validate()` in the format codec checks the AFM and then plays its
order through, from the start through one wrap of the loop, as the firmware
would:

- every stream decodes; setups, channels, relocations and lanes as in a flow;
  each stream has one `NEXT`, at its body length, then (for a new-play join)
  its tail, then `END`;
- at a new-play join the other executor must have ended its previous tail;
- the per-tick work of both executors together stays within the budget (38
  commands, 171 writes), and the header's work profile is its peak;
- the pool is the most voices held at once, and must equal the header's.

AICAforge's `afx_c_compile_song()` writes the AFM. Where a join tick would go
over budget it moves the outgoing tail's events later by whole ticks, keeping
their order, and reports what it moved; it refuses only a join that moving
cannot fit.

## API

```c
/* AICAflow, <aicaflow/song.h> */
int afx_song_load(const afx_bank_t *const banks[], uint32_t bank_count,
                  const void *afm, uint32_t afm_size, afx_asset_t *out_song);
int afx_song_seek(afx_instance_t instance, uint32_t order_position);
/* afx_instance_activate/stop/pause/rebuild/status, gain, tempo, lanes and the
 * end and loop callbacks take a song's instance like a flow's. */

/* AICAforge */
typedef struct {
    const afx_c_event_t *events; /* section-relative ticks, local channels */
    uint32_t count;
    uint32_t body_ticks;         /* NEXT */
    uint32_t end_ticks;          /* END; equal to body_ticks for a continue join */
    uint32_t join;               /* AFX_JOIN_NEW or AFX_JOIN_CONTINUE */
} afx_c_section_t;
int afx_c_compile_song(const afx_c_section_t *sections, uint32_t section_count,
                       const uint16_t *order, uint32_t order_count,
                       uint32_t loop_index /* AFX_C_SONG_ENDS */, uint32_t tick_rate,
                       const afx_c_zone_t *zones, uint32_t zone_count,
                       afx_c_output_t *out); /* AFB, AFM and AFV */
```

AFS scenes gain a song entry kind, so a scene places songs as it places flows.

## Plan

1. AICAflow: the format and validator, the firmware, the SH-4 song API, AFS
   song entries, `LOOP` removed; host and console checks.
2. AICAforge: `afx_c_compile_song`, `afx_compile --loop-tick` writing a
   continue-only song, `afx_scene` song entries, the loop entries removed.
3. The host backend (#22) follows main; Daytona's `afx_vgm` and SM64's
   converter target step 2.
