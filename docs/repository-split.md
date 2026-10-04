# AICAflow / AICAforge transition

AICAflow retains its repository identity and unmodified existing history.
[AICAforge](https://github.com/dfchil/AICAforge) owns offline authoring.
Dependency directories are named `dependencies/`, regardless of authorship.

| AICAflow keeps | AICAforge owns |
| --- | --- |
| SH4 runtime, ARM7 firmware, firmware manifest | Native compiler, optimizer and emitters |
| Canonical `format/` contract and specifications | Vendored `dependencies/aicaflow-format/` |
| Loader, validator and runtime tests | MIDI/SF2/PCM, N64 and VGM importers |
| Examples, DSP programs and enDjinn dependency | Bank/profile tools and authoring tests |
| Tuner server, Python client and their tests | Offline research utilities and authoring docs |

The portable codec validates asset bytes; firmware validation remains runtime
code. Firmware ABI, IPC, private addresses and runtime slot layouts are not
part of the authoring dependency. Format API 1 and disk versions AFB 1 / AFX 7 /
AFC 1 / AFI 1 remain unchanged.

## Checks

```sh
make check                              # runtime only; no AICAforge required
make -C /path/to/AICAforge check         # authoring belongs to AICAforge
make compatibility-check AICAFORGE_BIN=/absolute/AICAforge/build
```

`make check` is now runtime-only. Runtime checks include the checked-in firmware
hash and fixed assets produced before extraction, plus real loader tests through
the simulated transport. Integration adds current authoring output, repeatability,
bank merging, profiling, format invariants and bank/checkpoint binding rejection.
Rebuilding the firmware itself remains `make firmware-check` with the ARM toolchain.

Examples can use `make examples AICAFORGE_BIN=/absolute/AICAforge/build`.
The core runtime never compiles authoring code. Examples may generate assets
using external AICAforge binaries. The default location is an independently
cloned and built `dependencies/AICAforge/build/`; alternatively set
`AICAFORGE_BIN`. No compiler sources are duplicated in AICAflow.

## History-preserving extraction

`pre-repo-split` marks the known-good monorepo before codec/build separation.
`aicaforge-extraction-v1` pins the prepared extraction input.
Use git-filter-repo 2.47.0 in a maintainer environment:

```sh
sh scripts/extract-aicaforge.sh \
  https://github.com/dfchil/AICAflow.git aicaforge-extraction-v1 /new/path/AICAforge
git -C /new/path/AICAforge log --follow -- src/afx_compile_c.c
git -C /new/path/AICAforge blame src/afx_compile_c.c
```

The destination must not exist. Only a fresh clone is rewritten; AICAflow is
never filtered or force-pushed. The checked-in path list includes earlier file
locations, excludes the tuner/validator/DSP tools, and records every mapping.
Original authors, author dates and commit messages are retained; hashes change.
The generated `.git/filter-repo/commit-map` relates old and new commits.

The first standalone AICAforge commit adds its README, CI, format pin and update
script. The `repo-split-v1` tags in both repositories identify the initial
compatible transition pair; repository releases remain independent afterward.
The vendored format's `VERSION` records the immutable AICAflow source revision.

## Authoring copy removed

The initial `repo-split-v1` release retained a deprecated authoring copy.
At the owner's request, the follow-up cleanup removes that copy now rather
than extending the transition window. This changes the build workflow, not
the asset formats or runtime ABI.

`tools/author/`, `tools/research/`, authoring-only tests and duplicated
authoring documentation are now maintained only in AICAforge. The old
`make compiler` and `make authoring-check` targets have been removed.
Use `make -C /path/to/AICAforge` and `make -C /path/to/AICAforge check`.

The tuner and its tests stay in AICAflow. Compatibility tests invoke the
existing AICAforge CLI test suite instead of copying it back into this repo.
The small public format dependency is deliberately shared: AICAflow is its
canonical owner, and AICAforge pins a versioned copy.

Older workflows remain recoverable from `repo-split-v1`; history is not
rewritten. Git's history and existing build/download caches do not shrink
when source files are removed from the current checkout.
