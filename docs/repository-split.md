# AICAflow / AICAforge transition

AICAflow retains its repository identity and unmodified existing history.
[AICAforge](https://github.com/dfchil/AICAforge) owns offline authoring.
Dependency directories are named `dependencies/`, regardless of authorship.

| AICAflow keeps | AICAforge owns |
| --- | --- |
| SH4 runtime, ARM7 firmware, firmware manifest | Native compiler, optimizer and emitters |
| Canonical `format/` contract and specifications | Pinned `dependencies/AICAflow/` driver SDK |
| Loader, validator and runtime tests | MIDI/SF2/PCM, N64 and VGM importers |
| Examples, DSP programs and enDjinn dependency | Bank/profile tools and authoring tests |
| Tuner server, Python client and their tests | Offline research utilities and authoring docs |

The portable codec validates asset bytes; firmware validation remains runtime
code. Firmware ABI, IPC, private addresses and runtime slot layouts are not
included by the authoring compiler. The complete driver checkout is available
for integration tests, without making private runtime headers part of the compiler API. Format API 1 and disk versions AFB 1 / AFX 7 /
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
using external AICAforge binaries. The default toolchain is the pinned
`dependencies/AICAforge/` submodule; alternatively set `AICAFORGE_BIN`.
No compiler sources are copied into AICAflow.

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
That first release used a vendored format copy. The current design instead pins
the complete driver repository as a Git submodule; the copy and updater are removed.

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
The public format contract has one source location in AICAflow. AICAforge
uses it directly from its pinned AICAflow driver checkout.

Older workflows remain recoverable from `repo-split-v1`; history is not
rewritten. Git's history and existing build/download caches do not shrink
when source files are removed from the current checkout.

## Pinned dependencies without recursive builds

The build graph is **AICAflow driver → AICAforge → AICAflow examples**.

- AICAforge's `make dependencies` initializes only `dependencies/AICAflow/`.
  It does not initialize the SDK's enDjinn or AICAforge submodules.
- AICAflow's `make authoring-dependencies` initializes only its AICAforge
  submodule, then asks that toolchain to initialize its SDK.
- AICAflow's `make dependencies` additionally initializes its own enDjinn.
- Normal driver builds and `make check` need neither dependency.

Use these explicit targets, **not `--recurse-submodules`**. The AICAforge
submodule has `update = none` so generic submodule initialization skips it;
the example setup target explicitly opts in with `--checkout`.
Each gitlink pins an exact existing commit, not a moving branch. The SDK
revision used to build the tools may differ from the runtime under test;
CI checks both the pinned dependency pair and current cross-repo compatibility.

To update the examples' toolchain, start with a clean submodule:
```sh
make update-dependencies
make check compatibility-check
git add dependencies/AICAforge
git commit -m "Update example authoring toolchain"
```
The target follows AICAforge's `main`, then initializes that revision's
recorded SDK dependency. It does not advance the nested SDK independently.
enDjinn remains separately pinned; this update command does not change it.
Normal builds never fetch or update dependencies. `make dependencies` restores
the recorded revisions; commit tested gitlink changes to retain a new pairing.

AICAforge has the equivalent `make update-dependencies` for its direct driver
SDK dependency, following AICAflow's `main`. Update and commit there first if a
new driver should become the toolchain's baseline. Do not use recursive
`--remote` updates: that would discard the intended version pairing.
