# Tools and build targets

Native authoring now belongs in [AICAforge](https://github.com/dfchil/AICAforge).
The authoring/research copy here is deprecated during the transition; the tuner
and its Python client remain AICAflow-owned. See the
[migration guide](../docs/repository-split.md).

`make compiler` builds the supported native C authoring tools:

- `build/afx_compile` — one MIDI plus PCM zones or one SoundFont to AFB/AFX,
  with matching AFC/AFV sidecars.
- `build/afx_bank` — an editable `.afbm` map to one shared AFB and a flow
  plus sidecars for each declared song.
- `build/afx_profile` — initialise, inspect and apply an `.afp` performance
  profile to an AFX/AFC pair.
- `build/afx_demo_assets` — deterministic small assets for examples/tests.
- `build/afx_n64` — direct B1 ALBank/S1 CSeq music and ALBank SFX chains;
  no MIDI intermediate. SFX emit AFB/AFX; music also emits AFC/AFV.
- `build/afx_vgm` — Sega MultiPCM VGM/VGZ register captures to AFB/AFX/AFC/AFV.

The full workflows, input syntax and sidecar roles are in
[Authoring](../docs/authoring.md) and
[Assets and sidecars](../docs/specs/assets.md).

`afx_bank --create-map` makes an editable AFBM from MIDI/SF2 inputs;
`--per-song` builds independent banks; `--merge` losslessly deduplicates and
rebinds already-authored AFB/AFX sets. Only map-based builds emit compact and
named AFI catalogs. `afx_profile inventory` lists note selectors;
`init` creates a starting profile.

DKR's pack builder reads `.afsfx` maps and calls the C converter and merger.
See [SFX bank maps](../docs/specs/afsfx.md).

| Target | Output / requirement |
| --- | --- |
| `make compiler` | Native authoring binaries under `build/`; Clang, libm and zlib |
| `make tools` | Dreamcast persistent tuner; KOS SH4 environment |
| `make check` | Native/driver/Python tests; no ARM7 compiler |
| `make firmware-check` | Rebuilt firmware matched to its manifest; ARM7 toolchain |

`tuner/` contains the resident Dreamcast tuner (`server/`) and its small host
client (`client.py`). See [Tuner](../docs/tuner.md) for commands, asset lifetime
and reset behavior.

`author/` is the supported C toolchain; its [README](author/README.md) gives
command-line recipes. `test/` contains fixtures and checks. `research/`
contains experimental utilities, including the separate OoT AudioSeq reader.
Python is required by the tuner client and test suite.
