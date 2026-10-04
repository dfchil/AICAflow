# Runtime tools

- `tuner/`: persistent Dreamcast BBA server and Python client.
  Build with `make tools`; see [Tuner](../docs/tuner.md).
- `test/`: tuner client/server protocol tests, run by `make check`.

The asset validator is `driver/tools/afx_validate.c`; build it with
`make -C driver build/afx_validate`.

All native asset compilers, importers, bank/profile tools, authoring tests and
research utilities live in [AICAforge](https://github.com/dfchil/AICAforge).
See its [tool recipes](https://github.com/dfchil/AICAforge/blob/main/src/README.md).
