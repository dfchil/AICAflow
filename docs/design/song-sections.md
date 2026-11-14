# Design: songs made of sections

Status: proposal, 2026-11-14. Nothing is built yet. Daniel decided the
direction on 14 Nov (relayed by daytona-arcade-recomp-d2): the ARM7 firmware
plays a song as a sequence of sections, and `REPEAT` (AICAflow #24) is
dropped. This note gives the shape for review before building. The open
questions are at the end; one of them is whether `LOOP` goes, since SM64's
continuous sounds depend on it.

## Why sections

A song such as Daytona's t03 is intro, A, A, B, then back to the first A.
Daytona's arcade board allocates voices dynamically, so the second A cannot
reuse the first A's channels at the join: the first A's last notes are still
ringing when the second A starts. Each play of a section therefore runs as
its own executor with its own voices, and its ending rings out while the next
section starts. A section is stored once however often the order plays it.

## The song file (AFM)

An AICAforge tool writes one AFM file per song next to its AFB bank. It holds
what a flow holds today, once for the whole song, plus the sections and the
order:

- a header: magic, version, bank identities (slot 0 and, with
  `AFX_FLAG_BANKS`, slots 1 to 3), tick rate, the song's flags (`LANES`,
  `BANKS`), the section count, the order count, the loop index, the largest
  local channel count of any section, and the voice pool the song needs;
- one setup table and its relocations, shared by every section (SM64's
  template dictionary saves about 14% on songs; it is not duplicated per
  section);
- the lane map, if the song has lanes;
- the sections: each a stream and its authored body length in ticks;
- the order: section indices, and the loop index (the order position the song
  continues at after the last one; none for a song that ends).

A section's stream is today's bytecode with two additions and one rule:

- `NEXT` (no operands) marks the end of the section's body: in the tick it
  runs, the next section in the order starts on the song's other executor.
  It sits at the body length, so tempo scaling moves the join exactly as it
  moves every other event.
- `FREE channel` gives a local channel's voice back to the song's pool. The
  importer puts it after the channel's last release; a channel without one is
  given back when the section ends.
- The stream ends with `END`. The events between `NEXT` and `END` are the
  section's tail: its own voices' key-offs, level patches and releases,
  running past the start of the next section.

## A loaded song in AICA RAM

The song is one arena allocation (a scene can place it, as it places flows):

| Part | Written by | Contents |
|---|---|---|
| header, section table, order | SH-4, at load | offsets of the setups, lane map, bank table and each stream, relative to the allocation |
| setups, lane map, bank table, streams | SH-4, at load | as in a flow image |
| two executor blocks (128 B each) | SH-4 zeroes them before activation; then only the ARM7 | an `afx_runtime_slot_t` (pc, deadline, remaining wait, tempo carry, state) and the channel map, local to physical |
| control block (32 B) | ARM7 | order position, plays of the loop, the pool (granted voices and free voices) |

The split is that of `afx_observed_t`: after activation only the ARM7 writes
running state, and the SH-4 reads it for status. The ARM7 checks every offset
it follows against the song's allocation. The SH-4 frees the allocation only
after the song's end report, so the handle stays valid until every ringing
tail has ended.

The firmware's private memory gains nothing. A song takes one of the 64
instance slots, its handle; that slot's context holds the instance fields it
holds today (reference, state, sequence, gain, tempo, lanes) and the song's
address. Its two executors live in the song's allocation, not in the 64
runtime slots. (On main, 672 bytes of private memory are free.)

## The firmware

- **Executors.** The executor already works on a context pointer; a song's
  pass runs it on each of the song's two executor blocks, with the song's
  gain, tempo and lane gains, which therefore reach a ringing tail too.
- **A join.** At `NEXT` the control block advances the order position (to
  the loop index after the last, counting a loop; or nowhere, for a song that
  ends). The next section starts on the other block with the outgoing
  executor's deadline: the same tick, gapless, with no drift. Its local
  channels are mapped onto free pool voices at that moment. The outgoing
  executor keeps running its tail until `END`.
- **Voices.** The SH-4 grants the pool at activation, from the voices it
  hands out today, as many as the AFM header says the song needs. The ARM7
  never takes a voice outside it. `FREE` and `END` give voices back to the
  pool. Because the loader computes the pool exactly, a join never finds the
  pool short. Giving voices back to the SH-4 while the song plays, and asking
  for more ahead of dense passages, are later steps.
- **End.** A song without a loop index ends when its last section's executor
  ends; the end report then gives the whole pool back, as an ending flow does
  today.
- **Commands.** Gain, tempo, lanes, pause and stop take the song's handle and
  act on both executors. Pause keys off both and keeps the order position
  and the incoming section's executor, so a resume continues it; the tail
  that was ringing is not resumed.
- **Reports.** Each loop is reported as a `LOOP` is today (loop count in the
  state word, the loop callback). The status position names the order
  position and the offset in that section's stream of the newest executor.

## Load-time checks

The SH-4 validates the AFM before it uploads it, with the format codec:

- each stream decodes, has exactly one `NEXT` (none in the last section of a
  song that ends) at its body length, and ends with `END`;
- setup indices, channels and relocations as for a flow;
- **two executors suffice:** at every join in the order, the loop's included,
  the outgoing section's tail must end before the incoming section's own
  `NEXT`;
- **the work budget at a join:** the outgoing section's events in the join
  tick plus the incoming section's first burst stay within the per-tick
  budget (38 commands, 171 register writes), and the header's work profile
  covers it;
- **the pool:** the most voices held at any tick of any join (the tail's
  voices not yet given back, plus all of the incoming section's local
  channels) must equal the header's pool. For Daytona's t03 that is about 44
  (28 local channels plus about 16 still ringing).

## API

### AICAflow (SH-4)

```c
#include <aicaflow/song.h>
int afx_song_load(const afx_bank_t *const banks[], uint32_t bank_count,
                  const void *afm, uint32_t afm_size, afx_asset_t *out_song);
/* The pool this song needs; afx_instance_activate() grants it or returns
 * -AFX_NO_CHANNELS. */
uint32_t afx_song_voices(afx_asset_t song);
/* Restart at the start of the section at order position `position`. */
int afx_song_seek(afx_instance_t instance, uint32_t position);
typedef struct {
    uint32_t position;      /* order position of the newest section */
    uint32_t section;       /* its section index */
    uint32_t offset;        /* its stream offset */
    uint32_t loops;         /* times the order looped */
} afx_song_status_t;
int afx_song_status(afx_instance_t instance, afx_song_status_t *out);
```

`afx_instance_activate`, `_stop`, `_pause`, `_gain_set`, `_tempo_set`,
`afx_instance_lanes_set` and the end and loop callbacks take a song's
instance like a flow's. `afx_instance_rebuild` on a song takes a position
from `afx_song_status` (order position and stream offset) and resumes there,
which is how SM64 resumes after a pause today. AFS scenes gain a song entry,
so a scene places songs as it places flows.

### AICAforge

```c
typedef struct {
    const afx_c_event_t *events; /* section-relative ticks, local channels */
    uint32_t count;
    uint32_t body_ticks;         /* where NEXT goes: the next section starts */
    uint32_t end_ticks;          /* END, after the tail; >= body_ticks */
} afx_c_section_t;
#define AFX_C_SONG_ENDS UINT32_MAX
int afx_c_compile_song(const afx_c_section_t *sections, uint32_t section_count,
                       const uint16_t *order, uint32_t order_count,
                       uint32_t loop_index, uint32_t tick_rate,
                       const afx_c_zone_t *zones, uint32_t zone_count,
                       afx_c_output_t *out); /* AFB, AFM and AFV */
```

The events may include `AFX_OP_FREE` per channel; the compiler places
`NEXT` and `END`, builds the shared setup dictionary
(`afx_c_optimize_events`) across all sections, computes the pool and the work
profile, and refuses a song the loader would refuse. `afx_vgm` splits a
recording into sections, keeps the arcade's voice allocation within each, and
emits the order (Daytona's side).

## Open questions for Daniel

1. **Does `LOOP` really go?** SM64 has 114 continuous sounds (engines, wind,
   slides) that loop a stretch whose end has the same notes sounding as its
   start; the voice crosses `LOOP` without a new key-on. Sections cannot do
   that: each play starts new voices, so a held engine note would restart on
   every repeat. The options:
   - **(a) keep `LOOP` for plain flows** and use sections for songs. Nothing
     changes for continuous sounds, and there is no ABI cost, since it
     exists. This is my recommendation.
   - (b) remove `LOOP` and give sections a mode that continues on the same
     executor and channels. That is `LOOP` again under another name, inside
     a song, with more machinery.
   SM64's looping songs move to sections either way; the converter drops its
   cut-and-link workaround and emits the recorded tail after `NEXT`.
2. **`NEXT` in the stream, or only a body length in the table?** I propose
   the opcode: the join then follows tempo changes exactly, with no arithmetic
   in the firmware. The table keeps the body length for tools and checks.
3. **`FREE`, an opcode for early voice return.** Without it voices only come
   back at a section's `END`, and t03's pool grows from about 44 towards 56.
4. **Pause drops the ringing tail.** Resuming continues the newest section
   only. Fine for SM64's pause, as far as I can tell.
5. **Seek** goes to the start of an order position. Seeking inside a section
   would need a per-section AFC later.

## Plan

1. AICAflow, ABI 14 and format API 6: the AFM format and validator, `NEXT`
   and `FREE`, the song executor in the firmware, the SH-4 API, the host
   backend (#22) and `afx_render`, host tests, and a console check: a song of
   intro, A, A, B that loops into A, checking that each join is in the same
   tick, that tails ring past it, and that the pool comes back whole.
2. AICAforge: `afx_c_compile_song`, `afx_scene` song entries, and, depending
   on question 1, the `LOOP` compile entries.
3. Daytona: `afx_vgm` sections; SM64: the converter.

Each is its own PR. The console A/B for Daytona follows step 1.
