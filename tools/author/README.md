# Native authoring tools

This AICAflow copy is deprecated during the repository transition.
New authoring development belongs in [AICAforge](https://github.com/dfchil/AICAforge).

Build from the repository root:

```sh
make compiler
```

See [Authoring](../../docs/authoring.md) for workflows and
[Assets and sidecars](../../docs/specs/assets.md) for schemas and limits.

## MIDI and samples

```sh
# One PCM16 source, 44.1 kHz, root key 69:
build/afx_compile song.mid instrument.pcm song.afb song.afx
# Key-split PCM zones:
build/afx_compile song.mid --zones instrument.zones song.afb song.afx
# SoundFont presets selected by MIDI bank/program:
build/afx_compile song.mid --sf2 auto GeneralUser.sf2 song.afb song.afx
```

Each command emits AFB/AFX plus matching AFC seek and AFV visualizer sidecars.

## Bank maps

```sh
# Create an editable map from MIDI program usage:
build/afx_bank --create-map library.afbm GeneralUser.sf2 \
  title_theme midi/title_theme.mid field_theme midi/field_theme.mid
# Build one shared bank:
build/afx_bank library.afbm output music.afb
# Build one bank per song:
build/afx_bank --per-song library.afbm output
# Combine authored AFB/AFX pairs into one bank:
build/afx_bank --merge music.afb controls raw/sequence_*.afx
```

AFBM selects SF2 sources, presets, coding, rates and register-lowering policy.
Map builds emit compact and named AFI catalogs. Merge preserves encoded sample
bytes and relocates each AFX and optional AFC; it emits no AFI or AFV.
Standalone PCM uses `afx_compile --zones`, not AFBM.

## Performance profiles

```sh
build/afx_profile init song.afx song.afp room 112
build/afx_profile inventory song.afx
# Edit song.afp, then apply:
build/afx_profile apply song.afx song.afc song.afp song-performance.afx song-performance.afc
build/afx_compile --visual song-performance.afx song-performance.afv
build/afx_profile describe song.afx song.afp
```

AFP supports shared defaults, setup templates, note overrides and timed PATCH
lanes. It preserves NOTE/KEYOFF timing and reuses the bank. Changed profiles
produce an initial-only AFC checkpoint; SH4 replays events when seeking.
`describe` supplies DSP/send/tempo metadata for the application build.
The application installs DSP and applies instance tempo.

`export-lanes reference.afx base.afx profile.afp preset send [tempo_q8_8]`
extracts sustained PATCH expression into a profile bound to `base.afx`, matching
notes by source order. Both inputs use AFX file version 7.

## N64 CSeq and ALBank

```sh
build/afx_n64 audio_control.bin audio_table.bin sequences.bin 7 title_theme.afx
build/afx_n64 --sfx sfx_control.bin sfx_table.bin 563 collect_item.afx
```

Music imports B1 ALBank and S1 CSeq directly, producing AFB/AFX/AFC/AFV.
SFX imports DKR-style ALInstrument chains, producing AFB/AFX. SFX IDs are
one-based raw chain roots; finite components receive KEYOFF and sustained
components remain under SH4 STOP control.

SFX coding selects the smallest candidate passing 30 dB whole-sample and
24 dB attack SNR, at the source rate or lower 16/11.025/8 kHz rates. Looped
samples use PCM8 or PCM16. See [AFSFX](../../docs/specs/afsfx.md) for pack maps.
OoT AudioSeq uses a separate experimental reader.

## MultiPCM VGM/VGZ

```sh
build/afx_vgm track.vgz track.afx
build/afx_vgm track.vgz track.afx --gain-db -6.4 --adpcm
```

Outputs are AFB/AFX/AFC/AFV. Defaults are PCM8 and -6.4 dB gain.
The supported chip is Sega MultiPCM.
