
# Persistent tuner server

[Documentation](../../../docs/README.md)

The tuner receives assets and DSP programs over BBA while staying resident on
Dreamcast. Build from the repository root:

```sh
source /opt/toolchains/dc/kos/environ.sh
make tools
```

Load `tools/tuner/server/bin/afx_tuner_server.elf` once with a Dreamcast
loader, then use `tools/tuner/client.py` to upload an AFB, upload/play its
bank-bound AFX control flow, audition a region or change DSP state. `reset`
returns the tuner to a fully empty AICAflow state without relaunching it. The
complete command and memory-lifetime reference is in
[docs/tuner.md](../../../docs/tuner.md).
