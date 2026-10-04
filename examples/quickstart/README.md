# Quickstart

[Documentation](../../docs/README.md)

The smallest complete AICAflow program. It builds one generated sample bank
and one bank-bound flow, loads both from the executable, installs a room DSP
scene, plays the flow once, verifies lifecycle completion, then returns a
`PASS` or `FAIL` line on the Dreamcast console.

```sh
source /opt/toolchains/dc/kos/environ.sh
make -C examples/quickstart
make -C examples/quickstart bin/aicaflow_quickstart.cdi
```

Load `examples/quickstart/bin/aicaflow_quickstart.elf` with the normal
Dreamcast loader. It has no external assets or controller input.
