# Classical music player

An enDjinn player for three classical works, with a playlist, progress bar,
seek controls and AFV visualization. Each work has its own sample bank; only
the selected bank is resident in AICA RAM.

| Work | Default SoundFont | Sample policy | DSP |
| --- | --- | --- | --- |
| Chopin — Nocturne in C-sharp minor, Op. 27 No. 1 | GeneralUser GS, stereo | PCM16 | dry |
| Bach — Cello Suite No. 1 Prelude, BWV 1007 | Ethan Winer Cello Solo, stereo | 27 kHz PCM16 | `room_large` |
| Grieg — In the Hall of the Mountain King, Op. 46 No. 4 | GeneralUser GS, left channel | PCM8/ADPCM | `room` |

## Build and run

From the repository root:

```sh
source /opt/toolchains/dc/kos/environ.sh
make -C examples/music_player fetch-assets
make -C examples/music_player
kos-tool -f -t "$DCTOOL_HOST" \
  -m "$PWD/examples/music_player/cdrom/aicaflow_music_player" \
  -x "$PWD/examples/music_player/bin/aicaflow_music_player.elf"
```

Set `DCTOOL_HOST` to the Dreamcast's address. Keep the host server running and
the computer awake while using `/pc`. For a disc image:

```sh
make -C examples/music_player bin/aicaflow_music_player.cdi
```

D-pad up/down selects a song, A plays/pauses, B stops, left/right seeks ten
seconds, and L/R pages the list. START+A+B+X+Y exits through the enDjinn loop.

Hold X and press left/right to change speed in 10-percentage-point steps
(50–200% of the song's profile tempo, without changing pitch). Hold X and
press up/down to change playback volume in 5-percentage-point steps (0–100%).
Y resets both to 100%. Settings also work while paused or stopped and persist
across songs. The screen shows both values; the time display and spectrum
follow the chosen tempo. Volume scales the song's existing gain and dynamics;
100% is the original level, not additional amplification. DSP tails may ring
out briefly when the source volume is reduced to zero.

## Assets and profiles

[`classical.afbm`](classical.afbm) maps MIDI instruments to SoundFont sources
and selects sample coding, rate and channel. The builder includes only samples
used by the scores and emits AFB, AFX, AFC, AFV, AFI and named AFI files per work.

`profiles/*.afp` supplies timbre, DSP send, tempo and sustained expression.
Chopin uses gain-expression lanes; Bach and Grieg use shared DSP settings.
Bach's rubato and note timing are in the checked-in
[`performance/bach-bwv1007-prelude-performance.mid`](performance/bach-bwv1007-prelude-performance.mid).
The downloader verifies external inputs by SHA-256; the build verifies the
Bach performance MIDI and each applied AFP's base-file hash.

Grieg maps piccolo, flute, clarinet, bassoon, strings, brass and timpani.
Its exposed pizzicato, percussion and woodwind samples use `auto`: ADPCM must
pass full-sample and attack-window SNR checks, otherwise PCM8 is used.
Sustained string and brass layers use ADPCM. See
[Authoring](https://github.com/dfchil/AICAforge/blob/main/docs/authoring.md) for quality thresholds and bank-map options.

## Alternative sources

```sh
make -C examples/music_player \
  PIANO_SOUNDFONT=/path/to/piano.sf2 \
  CELLO_SOUNDFONT=/path/to/cello.sf2

make -C examples/music_player \
  GRIEG_MIDI='/path/to/orchestral-score.mid' \
  ORCHESTRA_SOUNDFONT=/path/to/orchestra-gm.sf2
```

Overrides can be used independently. `CELLO_SF2_PROGRAM` selects the cello
preset (default `0`). `PIANO_FORMAT` and `CELLO_FORMAT` accept `pcm16`, `pcm8`,
`adpcm` or `auto`. `PIANO_RATE`, `CELLO_RATE` and `ORCHESTRA_RATE` cap the
sample rate; `0` keeps the source rate. Piano and orchestra default to `0`,
cello to `27000`. Grieg's coding choices are in the map.

`PIANO_CHANNEL`, `CELLO_CHANNEL` and `ORCHESTRA_CHANNEL` accept `stereo`,
`left` or `right`. A single linked side is centered on AICA and uses half the
paired sample memory. Large banks may require PCM8 or a reduced sample rate:

```sh
make -C examples/music_player \
  PIANO_SOUNDFONT=/path/to/piano.sf2 PIANO_FORMAT=pcm8 PIANO_CHANNEL=left
```

A changed source can invalidate its AFP hash. The build then uses the base
flow with dry DSP and authored tempo. Create a profile against the new AFX to
restore performance settings; see [Authoring](https://github.com/dfchil/AICAforge/blob/main/docs/authoring.md#performance-profiles).

## Source licences

External MIDI and SF2 files are downloaded inputs, not distributed assets:

- Chopin MIDI: Bernd Krueger, [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/),
  according to [the source listing](https://piano.center/midi/nocturne-op-27-no-1-in-c-sharp-minor).
- Bach score: [Mutopia, public domain](https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=517).
  The performance MIDI in this repository is derived from that score.
- Grieg MIDI: Hans-Joachim Roeder's orchestration from
  [MidiCities](https://midicities.com/geocities-browse?title=4).
- GeneralUser GS: [upstream licence](https://github.com/ad-si/GeneralUser/blob/master/LICENSE.txt).
- Cello Solo: Ethan Winer's royalty-free SoundFont.

Use suitably licensed SoundFonts for replacements. See `fetch_assets.sh` for
input URLs and hashes, and [Asset licences](../../ASSET_LICENSES.md).

## Hardware frame test

The frame test loads and plays all works without controller input, checks
that visualization starts, prints `CLASSICAL PLAYER FRAME TEST PASS`, then exits:

```sh
source /opt/toolchains/dc/kos/environ.sh
make -C examples/music_player frame-test
kos-tool -f -t "$DCTOOL_HOST" \
  -m "$PWD/examples/music_player/cdrom/aicaflow_music_player" \
  -x "$PWD/examples/music_player/bin/aicaflow_music_player.elf"
```

This leaves a self-terminating ELF. Run `make -C examples/music_player -B`
before an interactive listening session.
