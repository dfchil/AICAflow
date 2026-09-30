# Programmable DSP demo

This self-contained Dreamcast example generates a short sine-note flow,
installs the public `pingpong` DSP program at runtime, then plays the flow.
The four hits alternate through a 180 ms stereo echo. It uses the DKR Edition
host API directly; it has no dependency on the DKR game or an external example
framework.

```sh
source /opt/toolchains/dc/kos/environ.sh
python3 -m pip install mido
make -C examples/dsp_demo
```

Load `examples/dsp_demo/bin/aicaflow_dsp_demo.elf` with the normal Dreamcast
loader. The serial console prints `PASS` after the flow completes.
