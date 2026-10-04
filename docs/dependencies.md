# Dependencies

[Documentation](README.md)

AICAflow contains the SH4 driver, ARM7 firmware, public runtime formats,
DSP programs, examples and tuner. [AICAforge](https://github.com/dfchil/AICAforge)
contains the importers, asset compilers, bank/profile tools and authoring documentation.

AICAforge compiles the portable code in `driver/format/` from its pinned
AICAflow SDK. It does not include private firmware or IPC headers.

## Setup

```sh
make dependencies             # Example toolchain and enDjinn
make authoring-dependencies   # Toolchain only
```

Dependencies are pinned Git submodules. These targets initialize AICAforge
and its recorded AICAflow SDK without initializing the SDK's nested dependencies.
Use them instead of recursive submodule checkout.

Driver builds and `make check` need neither AICAforge nor enDjinn.
Examples use `dependencies/AICAforge/` by default; set
`AICAFORGE_BIN=/absolute/AICAforge/build` to use another build.

## Update

Start with clean submodules:

```sh
make update-dependencies
make check compatibility-check
git add dependencies/AICAforge
git commit -m "Update example authoring toolchain"
```

This follows AICAforge's `main` and initializes its recorded SDK revision.
It does not update enDjinn or advance the nested SDK independently.
Normal builds do not fetch dependencies; `make dependencies` restores the pins.

To update AICAforge's SDK, run its `make update-dependencies`, test and commit
there first. Do not use recursive `--remote` updates.

See [Testing](testing.md) for host, compatibility and hardware checks.
