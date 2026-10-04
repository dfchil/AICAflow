# AICAflow

AICAflow is a Dreamcast audio runtime for the Yamaha AICA.
[AICAforge](https://github.com/dfchil/AICAforge) is its standalone native
authoring toolchain: MIDI/SoundFonts, raw PCM, N64 CSeq/ALBank and Sega MultiPCM
VGM/VGZ become sample banks and timed AICA register flows.
SH4 owns AICA RAM, asset validation, bank binding, instances, seeking, live
controls and DSP scenes. ARM7 schedules prepared commands and writes AICA
registers.

## Playback model

- One sampled-flow model: one AFB plus one AFX. AFX never embeds samples.
- One versioned firmware image: `firmware/aicaflow.drv`.
- AFB/AFX validation rejects incompatible banks and malformed relocations
  before an instance can play.

## Build an example

```sh
git clone --recurse-submodules https://github.com/dfchil/aicaflow.git
cd aicaflow
source /opt/toolchains/dc/kos/environ.sh
make examples
```

The checked-in firmware means this needs no ARM7 compiler. Run `make check` for
host validation; maintainers with the ARM toolchain run `make firmware-check`
to reproduce the release image.

`make compiler` temporarily retains the deprecated monorepo authoring workflow.
For new integrations, build AICAforge and pass
`AICAFORGE_BIN=/absolute/AICAforge/build` when building examples.
See the [migration and test guide](docs/repository-split.md).
`make runtime-check` requires neither AICAforge nor a Dreamcast toolchain.

## Files and ownership

| File | Purpose | Used where |
| --- | --- | --- |
| `.afb` | Encoded sample bank | Payload in AICA RAM |
| `.afx` | Timed register commands and reusable note setups | Image in AICA RAM |
| `.afc` | Optional seek checkpoints | SH4 RAM only |
| `.afv` | Optional visualizer animation | Player only |
| `.afi` | Optional binary sample catalog | SH4 code only |
| `.afbm` | SoundFont/MIDI bank-building map | Offline |
| `.afp` | Timbre, DSP and performance adjustments | Offline |
| `.afsfx` | SFX grouping/residency map | Offline DKR pack builder |

Several flows can share one resident bank. A flow always binds to exactly one
bank; it does not contain samples or look them up by instrument name at runtime.
An AFP rewrites AFX register commands. AFC checkpoints stay in SH4 RAM. See the
[format reference](docs/specs/assets.md) for layouts, bindings and limits.

## Included examples

- `quickstart` — minimal generated AFB/AFX playback.
- `multiple_dsp_effects` — enDjinn-guided separate and simultaneous dual-effect audition.
- `dsp_effects_player` — interactive DSP-preset audition player.
- `dynamic_sfx` — SH4-controlled pitch, position and intensity changes.
- `music_player` — three reproducibly fetched classical MIDI/SoundFont demonstrations.

The persistent BBA tuner is a development tool at `tools/tuner/server`; build
it with `make tools`.

Interactive examples use the pinned `dependencies/enDjinn` submodule. The core
driver does not. `music_player` downloads its declared MIDI and SoundFont
inputs on its first build; see its README to supply a different SoundFont.

## Documentation

See the [documentation index](docs/README.md) for guides and specifications.
Asset licences are in [ASSET_LICENSES.md](ASSET_LICENSES.md).

The reusable CSeq/ALBank and MultiPCM importers are included. DKR's game
integration, ROM extraction, SFX residency policy and bonus soundtrack player
live in the DKR repository. Game ROMs, extracted Nintendo/Sega samples and
soundtracks are not distributed here. OoT AudioSeq uses a separate experimental
research reader.
