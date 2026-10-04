# Assets and sidecars

## One playback model

Every sampled flow is one **AFB** sample bank plus one **AFX** control flow.
Several AFX files may bind to the same AFB, but an individual AFX never binds
to more than one bank. If a piece needs material from several SoundFonts or
sample sets, the offline tool combines the selected samples into that one AFB.

At runtime the SH-4 loads the AFB payload as one contiguous AICA allocation,
then validates and uploads AFX images that carry the bank's identity. The AFB
has no runtime sample name table or lookup index: each AFX setup already holds
the required bank-relative address and AICA register values.

| Extension | Role | Read by |
| --- | --- | --- |
| `.afb` | Sample bank; one contiguous encoded payload | SH-4 loader |
| `.afx` | Timed control stream and AICA setup templates | SH-4 loader, ARM7 executor |
| `.afc` | Optional seek checkpoints for one exact AFX/AFB pair | SH-4 only |
| `.afv` | Optional player visualisation frames | Player only |
| `.afp` | Offline performance/DSP profile | Offline authoring tool |
| `.afbm` | Editable source map for building a shared AFB | Offline bank builder |
| `.afi` | Optional binary AFB sample catalog | SH4-side one-shot code |
| `.afsfx` | Application SFX grouping/residency map | Offline application pack builder |

AFBM, AFP and AFSFX are offline authoring inputs. Binary files use
little-endian fields and fixed headers.

## AFB — sample bank

An AFB begins with exactly 32 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `AFB\0` |
| 4 | 4 | version (`1`) |
| 8 | 8 | bank identity: low then high 32-bit word |
| 16 | 4 | payload offset (`32`, 32-byte aligned) |
| 20 | 4 | payload bytes |
| 24 | 4 | total file bytes |
| 28 | 4 | reserved, zero |

The payload is the exact encoded sample block copied into one AICA asset.
Individual samples may use PCM16, PCM8 or AICA ADPCM in the same bank; their
format, loop points and offsets are already represented in the AFX setup
records. The runtime recomputes neither a hash nor a checksum over sample data;
it compares the precomputed identity in the AFB and AFX headers.

## AFX — control flow

An AFX begins with an 80-byte header (`AFX2`, file version `7`). The upload
image starts at a 32-byte-aligned file offset.

All fields are unsigned 32-bit little-endian words:

| Offset | Field | Meaning |
| ---: | --- | --- |
| 0 | magic | `AFX2` |
| 4 | abi | AFX file version `7`, not firmware IPC ABI |
| 8 | total_size | Complete file bytes |
| 12 | flags | Controlled/music/lane flags defined in `format.h` |
| 16 | image_offset | File-relative start of the 32-byte-aligned upload image |
| 20 | image_size | Upload image bytes |
| 24 | stream_offset | Image-relative bytecode start |
| 28 | stream_size | Bytecode bytes, including final END/PARK |
| 32 | control_id | Nonzero identity of the exact control image |
| 36 | setup_count | Number of 36-byte register setups |
| 40, 44 | bank_id_low, bank_id_high | Identity of the sole bound AFB |
| 48 | relocations_offset | File-relative relocation-table start |
| 52 | relocation_count | Number of 12-byte relocation records |
| 56, 60 | reserved0, reserved1 | Zero |
| 64 | required_channels | Local voice count required by this flow |
| 68, 72 | tick_rate_num, tick_rate_den | Authored ticks per second = numerator / denominator |
| 76 | work_profile | Peak burst commands in high 16 bits, register writes in low 16 bits |

The file header and relocation table are SH4 loader data; only the resolved
image is stored in AICA RAM. Setups contain format, loop and address words
bound to the separate AFB.

Each relocation gives a setup-pair offset, a bank-relative sample offset and
its byte length. The loader checks it against the AFB payload and resolves the
address once. AFX contains no sample data or names.
Each 12-byte on-disk relocation is `pair_offset`, `sample_offset`,
`sample_bytes` (three u32 words). `pair_offset` is image-relative and points to
the setup CONTROL/SAMPLE_LOW pair; `sample_offset` is **AFB-payload-relative**,
not a sample index or a file offset. The validator in `format/src/codec.c`
is authoritative for ranges, flags and work-profile constraints.

`control_id` identifies the exact control image. It changes when an offline
profile derives a new AFX and lets its AFC sidecar be rejected if stale.

## AFC — seek sidecar

An AFC begins with exactly 32 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `AFC\0` |
| 4 | 4 | version (`1`) |
| 8 | 4 | AFX control identity |
| 12 | 8 | AFB identity: low then high word |
| 20 | 4 | payload offset (`32`) |
| 24 | 4 | payload bytes |
| 28 | 4 | total file bytes |

The payload is a checkpoint table used only by the SH-4 when a player offers
seek. Normal playback does not need AFC. SH4 validates the header and retains
only the checkpoint payload. The file is never uploaded as an AICA asset.
An AFC file-size display includes its 32-byte header; it is not AICA RAM usage.

### Checkpoint payload

The payload begins with four u32 words: magic `CKP1`, checkpoint version `1`,
entry count, and reserved zero. Entries follow consecutively, without a
secondary chunk container. Each starts with four u32 words:

| Entry-relative offset | Meaning |
| ---: | --- |
| 0 | Authored checkpoint tick |
| 4 | Image-relative stream position |
| 8 | Authored ticks remaining until the next due stream operation |
| 12 | Active channel-state count |

Each channel state is 40 bytes: u32 local-channel number, followed by the 18
u16 register fields in AFX field order. Sample addresses remain bank-relative
in the file. The normal C emitter records checkpoints at 1000-authored-tick
intervals. This is about one second at 1000 Hz, not a fixed wall-clock interval
for every authored rate or runtime tempo.

SH4 selects the preceding checkpoint, replays ordinary events to the target,
resolves bank addresses and sends prepared register states via REBUILD. That
operation can use temporary AICA staging memory; it does **not** load the AFC
table into AICA or make ARM7 interpret it. Checkpoints describe register/timeline
state, not a saved PCM decoder cursor, sample waveform or DSP delay-ring image;
seeking is musical reconstruction, not bit-exact sample-phase restoration.

An AFP register transform emits a tick-zero checkpoint; later seeks require
SH4 event replay. An empty transform retains the AFC byte-for-byte.

## AFV — visualisation sidecar

AFV stores 32 one-byte bands per frame at 60 Hz. Values represent active-note
pitch and level with a short visual decay, normalized across the piece's
pitch range. It is a note-energy animation, not an FFT of the output audio.
AFV is optional and does not affect playback or seeking.
The exact 12-byte header is `magic[4]`, `version:u8=1`, `bands:u8=32`,
`rate:u8=60`, `reserved:u8=0`, `frames:u32`. It is followed by `frames * bands`
bytes in frame-major order. Regenerate AFV after changing the AFX.

## AFP — performance profile

AFP is editable JSON and is bound to one base AFX by its AFX file version and a
SHA-256 digest. It is an offline transform: it writes a derived AFX and matching
AFC. Note timing belongs to the source
MIDI/control flow; AFP is for timbre, articulation, DSP routing and an optional
whole-flow playback rate.

```json
{
  "format": "aicaflow.afp",
  "version": 4,
  "base": { "afx_version": 7, "canonical_sha256": "..." },
  "dsp": { "preset": "room_warm" },
  "tempo_q8_8": 220,
  "defaults": { "dsp_send": 128 },
  "templates": {
    "cello": { "parameters": { "lfo": 19024 } },
    "close": { "parameters": { "dsp_send": 0 } }
  },
  "setup_templates": { "0": "cello" },
  "overrides": [
    { "event": { "kind": "note", "tick": 750, "ordinal": 0, "channel": 0 },
      "template": "close" }
  ],
  "lanes": [
    { "event": { "kind": "note", "tick": 750, "ordinal": 0, "channel": 0 },
      "offset": 180, "parameters": { "mix": 32007 } }
  ]
}
```

The effective parameters for a note are resolved in this order:

`base AFX setup → defaults → setup template → note template → note parameters`.

An ordinal is the zero-based position among note events at the same tick; it is
only an offline selector, never a timing offset. `build/afx_profile inventory
song.afx` prints the available selectors and their source setups. `init` writes
defaults and empty template, override and lane collections.

A `lanes` entry is a timed register change belonging to one selected note. Its
`offset` is measured from that note's NOTE-on tick. An offset of zero is folded
into that NOTE before key-on, so it costs no PATCH; use an override, template or
default for new authored note-start settings. A positive offset emits a normal
AFX `PATCH` while the voice is active. Lanes change articulation or expression,
not the NOTE/KEYOFF schedule: timing and humanisation still belong in MIDI or
another control-flow source. They are compiled offline, so the ARM7 receives no
profile format or lane interpreter.

`dsp.preset` selects **one scene for the whole flow**, installed by the SH4
application. `dsp_send` is an AICA
channel register: a global default can send every note to that scene, a template
can change the send for a family of notes, and one override can make a selected
note drier or wetter. The same precedence works for `env_ad`, `env_dr`, `lfo`,
`direct`, `mix`, `filter_level0` through `filter_level4`, `filter_ad` and
`filter_dr`. These are raw 16-bit AICA register words so the profile lowers to
existing `NOTE` and `PATCH` commands; later source patches cannot overwrite a
profiled field while that note is active.

`tempo_q8_8` is optional and applies to the whole flow: `256` is authored
tempo, `128` is half speed and `512` is double speed. The SH4 sends that
existing instance-tempo value when it activates the flow, so it changes neither
the AFX command stream nor the individual NOTE/KEYOFF offsets. It is useful for
choosing an overall performance pace, not for rhythmic humanisation.
The offline build reads `afx_profile describe` into player/application
metadata. The preset name and tempo are not embedded as a runtime profile in
AFX; merely uploading the derived AFX does not install DSP or set that speed.

Supported preset names are `dry`, `room`, `room_warm` and `room_large`.
If no register words change, applying the profile copies AFX and AFC unchanged.

### AFP reference

All top-level properties shown in the JSON example are required, except
`tempo_q8_8`, which defaults to `256`. `base.afx_version` must be `7` and
`base.canonical_sha256` must be the exact SHA-256 of the input AFX; a mismatch
is an error.
Template names are non-empty strings of at most 47 bytes; at most 64 templates
are accepted. Every `setup_templates` key is a decimal setup number present in
the base AFX and every referenced template must exist.

Every `parameters` object accepts one or more of these unsigned 16-bit AICA
words. The same parameter object is used by `defaults`, templates, overrides
and lanes:

| Key | What it controls |
| --- | --- |
| `env_ad`, `env_dr` | amplitude-envelope attack/decay/release word |
| `lfo` | pitch/amplitude LFO word |
| `dsp_send` | per-voice DSP send level/routing word |
| `direct` | direct-path, pan and filter-routing word |
| `mix` | total-level/mix word |
| `filter_level0` … `filter_level4` | filter-envelope level words |
| `filter_ad`, `filter_dr` | filter-envelope rate words |

`setup_templates` applies a named template to every NOTE using that source
setup. `overrides` then selects one NOTE and may add a template and/or direct
parameters. `lanes` selects one NOTE and applies parameters at a relative
offset. Selector identity is always exactly:

```json
{ "kind": "note", "tick": 750, "ordinal": 0, "channel": 0 }
```

The parser rejects missing selectors, unknown parameter names, values outside
`0..65535`, unused overrides/lanes, a lane after its target has KEYOFFed, and
per-tick command/write-budget overflows.

## AFBM — bank map

AFBM is a human-edited text input to `afx_bank`, not a runtime asset. It
combines several MIDI files and SoundFonts into one shared bank while keeping
one bank binding per generated flow:

```text
source gm        soundfonts/GeneralUser.sf2
source orchestra soundfonts/orchestra.sf2 stereo

# map <song|*> <midi-bank> <midi-program> <source> <sf2-bank> <sf2-program> <format> [key=value ...]
map * 0 0  gm        0 0  auto rate=22050
map * 0 42 orchestra 0 42 pcm16 rate=44100 filter=static gain=fluidsynth2

song title_theme midi/title_theme.mid [control-tick-rate] [release-tail-ms]
song field_theme midi/field_theme.mid

# Optional deterministic offline variation for one song:
humanize title_theme 20260909 30 8 0
```

`source` names an SF2 input and may end with `stereo` (the default), `left`,
or `right`. The latter two select one linked side and centre it on AICA, which
is useful when a detailed stereo library will not fit in RAM. `map` routes a
MIDI bank/program to an SF2 preset and chooses `pcm16`, `pcm8`, `adpcm` or
`auto`; a song-specific map overrides `*`. `song` selects the MIDI sources.
An optional `midi_channel=<0..15>` narrows a map to its source MIDI channel;
that map wins over the generic rule for the same song and program. This is an
offline source-selection rule, not a runtime voice-channel assignment.
`auto` uses the deterministic offline quality gate and conservatively avoids
ADPCM for looped sources. The named `map` options are `rate`, `filter_offset`,
`filter`, `gain`, `gain_bias`, `envelope`, `source_pan`, `source_reverb`,
`dsp_send`, `modulators`, `midi_channel`, `direct`, `lfo` and `loop_ms`; their accepted values and defaults
are defined in [Authoring](https://github.com/dfchil/AICAforge/blob/main/docs/authoring.md). A fourth `song` value reserves a post-KEYOFF release
tail on AICA without extending the control stream's musical timing; it is an
offline voice-allocation constraint, useful for SF2 piano and string releases.
The bank builder writes the shared `.afb` plus one `.afx`, `.afc` and `.afv`
for each song. `--create-map` creates an editable starting AFBM from an SF2
and MIDI files.

`humanize <song> <seed> <level-centibels> <shorten-ms> <lfo-rate-steps>` is
an optional deterministic offline score-lowering directive. It changes NOTE
level and KEYOFF placement before AFX emission; it creates no runtime state.
`lfo-rate-steps` must be `0`; rate variation is not implemented. Put deliberate
rhythmic timing in MIDI.

### AFBM reference

AFBM is line-oriented UTF-8 text. Blank lines and lines whose first
non-whitespace character is `#` are ignored; inline comments are not syntax.
Paths are resolved relative to the AFBM itself. Quote a `source` or `song`
path when it contains spaces. All `source` lines must precede all `map` lines,
and all maps must precede the first `song` line.

AFBM `source` accepts SF2 files. Standalone PCM uses `afx_compile --zones`.

| Directive | Grammar | Meaning |
| --- | --- | --- |
| `source` | `source <name> <sf2-path> [stereo|left|right]` | Names one SF2 input. `stereo` is default; `left` and `right` select one linked side and centre it. |
| `map` | `map <song|*> <midi-bank> <midi-program> <source> <sf2-bank> <sf2-program> <format> [option…]` | Routes matching MIDI notes to a preset in a named source. Banks are `0..16383`; programs are `0..127`. |
| `song` | `song <name> <midi-path> [tick-rate] [release-tail-ms]` | Adds a flow. Tick rate defaults to `1000` (`1..1000000`); release tail defaults to `0` (`0..10000`). |
| `humanize` | `humanize <song> <seed> <level-centibels> <shorten-ms> 0` | Deterministic level variation `0..120` centibels and KEYOFF shortening `0..20` ms. Must appear after the song. |

`<format>` is `pcm16`, `pcm8`, `adpcm` or `auto`. `auto` first accepts ADPCM
only when the deterministic whole-sample and attack-window quality tests pass;
otherwise it emits PCM8. Looped material uses PCM8.

Optional map settings:

| Option | Values and default | Effect |
| --- | --- | --- |
| `rate` | `0..192000`; `0` | Caps decoded sample rate; zero keeps the SF2 rate. |
| `filter_offset` | `-16000..16000`; `0` | Adds SF2 cents before lowering the AICA filter. |
| `filter` | `envelope` (default), `static`, `none` | Chooses lowered SF2 filter treatment. |
| `gain` | `standard` (default), `fluidsynth2` | Chooses SF2 attenuation calibration. |
| `gain_bias` | `-10200..10200`; `0` | Adds offline gain calibration in centibels. |
| `envelope` | `sf2` (default), `fixed` | Uses the SF2 amplitude envelope or a fixed AICA envelope. |
| `source_pan` | `apply` (default), `ignore` | Applies or suppresses SF2 and MIDI pan. |
| `source_reverb` | `apply`, `ignore` (default) | Converts SF2 reverb send to DSP send. |
| `dsp_send` | `0..255` | Sets an explicit DSP send; cannot be combined with `source_reverb=apply`. |
| `modulators` | `apply` (default), `ignore` | Samples supported linear SF2 modulators at NOTE-on and lowers them into ordinary setup/NOTE data. |
| `midi_channel` | `0..15` | Narrows this route to one MIDI input channel. |
| `direct` | `0..65535` | Sets the raw AICA direct-path word. |
| `lfo` | `<rate>,<pitch-depth>,<amplitude-depth>`; `rate 0..31`, depths `0..7` | Sets one static raw AICA LFO word. |
| `loop_ms` | `20..2000` | Trims a looped SF2 sample near this duration at a low-discontinuity point. |

Routing chooses, in order: song-specific channel map, song-specific generic
map, `*` channel map, then `*` generic map.

`build/afx_bank map.afbm out shared.afb` writes one shared AFB with AFI catalogs,
plus AFX/AFC/AFV per song. `build/afx_bank --per-song map.afbm out` writes an
independent bank and sidecars per song.

The native import path lowers static SF2 envelope, attenuation, pan,
reverb-send and filter controls, plus supported linear SF2 modulator graph
values sampled at NOTE-on, into the existing AICA setup words and NOTE levels.
Curved, LFO and other time-varying graph sources are not approximated; use an
explicit AFP lane if they need an authored AICA PATCH. AFX never embeds SF2
data or a generic modulator interpreter.

## AFI — SH4 sample catalog

AFI is the optional binary catalog that lets SH4-side code start AFB samples
as direct one-shots without scanning an AFX setup dictionary. It is never
uploaded to AICA or interpreted by ARM7. The bank builder emits two variants
next to every AFB: `bank.afi` has compact records and `bank.names.afi` adds
fixed-width source sample names. Both bind to exactly the same AFB.

An AFI begins with a fixed, 32-byte little-endian header:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `AFI\0` |
| 4 | 4 | version (`1`) |
| 8 | 8 | AFB identity: low then high word |
| 16 | 4 | record offset (`32`) |
| 20 | 4 | unique sample count |
| 24 | 4 | record bytes (`16` or `32`) |
| 28 | 4 | total file bytes, padded to 32 bytes |

Each compact 16-byte record is little-endian:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | sample **file offset** in the AFB |
| 4 | 4 | effective sample rate in Hz |
| 8 | 4 | decoded sample length in frames |
| 12 | 1 | AICA format (`0` PCM16, `1` PCM8, `2` ADPCM) |
| 13 | 3 | reserved, zero |

The 32-byte named record appends a zero-padded, fixed 16-byte source sample
name at offset 16. Duplicate AFX setups that refer to the same AFB sample
produce one AFI record. AFI describes direct one-shots; loop points, root key
and tuning are stored in AFX setups.

## AFSFX — SFX bank grouping/residency map

An AFSFX declares which source sounds should be packed/preloaded together.
It does not describe PCM coding, a sample catalog, a DSP program or a control
stream. The reader is DKR's `dreamcast/build_aicaflow_sfx.py`; the
native `afx_n64 --sfx` and `afx_bank --merge` perform conversion and packing.
Neither the AICAflow runtime nor `afx_bank` parses AFSFX directly.

See [SFX bank maps](https://github.com/dfchil/AICAforge/blob/main/docs/specs/afsfx.md) for grammar, IDs, outputs and residency rules.
