# Design: streaming a song's samples by plan

Status: an idea for later, not built. Measured on 2026-11-14 for Daytona's
t03 (daytona-arcade-recomp-d2, from Daniel's idea). Set aside because Daytona's
Beginner scene fit without it, once the samples whose ADPCM noise the song hides
were coded as ADPCM and the race bank lost the effects the game never sends.

## The idea

A song's samples are most of its AICA RAM. Daytona's t03 holds 1,184,748 bytes
of PCM8 samples against 153,113 bytes of commands. But music playback is
deterministic: the compiler knows, for every moment of the song, which samples
the next notes read. So a song need not keep all its samples resident. A plan,
computed offline, could stream each sample into AICA RAM some time before its
first note in a stretch of the song, and give its space back once the stretch
has passed. The SH-4 would follow the plan by the song's position.

## What it would save

t03 plays intro, A, A, B and loops into A. Cut into segments, with the playing
segment's samples and the next segment's resident (a note holds its sample for
3 s after its start):

| Segments | Peak sample RAM | Saved | Largest load, and the time it has |
|---|---|---|---|
| none (all resident, today) | 1,184 KB | – | – |
| the sections (intro, A, B) | 1,174 KB | 9 KB | 669 KB in 105 s |
| 30 s | 882 KB | 290 KB | 387 KB in 30 s |
| 10–15 s | 785 KB | 390 KB | 234 KB in 10 s |
| 5 s | 719 KB | 450 KB | 228 KB in 5 s |

Whole sections save almost nothing: A needs 41 of the 49 samples and plays for
105 s. The gain comes from cutting A into stretches that each use only some of
its instruments. A 6 s note hold gives the same figures from 10 s segments up.
The commands are small beside the samples; streaming them per section is
possible but not where the space is.

## How it could work

- **Fixed addresses, no allocator at run time.** The compiler knows each
  sample's lifetimes over the loop. It can give every sample one AICA address
  for all of them, and let samples whose lifetimes never overlap share space
  (interval colouring of a cyclic schedule). Notes and setups then never
  change; the firmware stays as it is; nothing can fragment. Fixed addresses
  cost something above the ideal peak in the table; the compiler would report
  how much.
- **The plan travels with the song.** A list of (song position, sample,
  address) loads in the AFM, or beside it; freeing is implied by the next load
  into the same space. Like sections and `trim`, it would be chosen in the bank
  map (a segment length, say), not on a command line.
- **The SH-4 follows the song's position.** `afx_instance_status` already
  reports a song's order position and stream offset. When the song passes a
  plan entry's position, the host starts that load. At 10 s segments the
  biggest load is 234 KB with 10 s to arrive, about 25 KB/s: far below the G2
  bus and the GD-ROM.
- **Where the samples wait.** Either the SH-4 keeps the whole bank in its own
  RAM (simple, but 1.2 MB of SH-4 heap is a lot in a race), or a reader streams
  them from disc as the plan needs them (little SH-4 RAM, one more reader in
  the race; the AICAflow slot-stream DMA path already moves bank data in
  pieces).

## Open questions

- A late load: the plan must say what a note does when its sample has not
  arrived (keep the space's previous sample, or skip the note), and the host
  should report it.
- Pause, seek and `afx_instance_rebuild()` must restore the plan's state at a
  position, not only the executors'.
- Fixed-address packing over a loop: how far above the ideal peak it lands for
  real songs.
- Two songs or a song and effects sharing the RAM a plan frees between peaks.

## How to measure a song

The t03 figures came from a scratch tool that prints every NOTE's tick and
sample (afx_vgm's lowered events) and each sample's trimmed size, and a script
that walks the song's order in segments and takes the peak of the playing and
next segment's samples. A compiler pass would compute the same from the song's
own events and report it with the AFM.
