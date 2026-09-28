# AICAflow DKR Edition: integration

DKR Edition is a fixed runtime and offline N64 import pipeline for the
Dreamcast Diddy Kong Racing port. It is not the general AICAflow toolkit.

DKR keeps `third_party/aicaflow` at a tested release tag. Initialise it with:

```sh
git submodule update --init --recursive
source ../enDJinn/environ.sh
python3 -m pip install mido
make -f Makefile.dc -j8 dkracing.elf
```

`Makefile.dc` builds the ARM7 firmware and SH-4 static library, imports the
N64 music/SFX assets, and links `dkracing.elf`. `make -f Makefile.dc -j8 cdi`
uses the same generated audio files with the `/cd` mount.

## Runtime order

The public API is in `driver/sh4/include/aicaflow/host.h`; the bank loader API
is in `driver/sh4/include/aicaflow/bank.h`.

1. Embed `driver/arm7/aicaflow.drv`, then call `afx_init` once.
2. Load each required AFB once with `afx_bank_load_file`, then upload its
   sample-free AFX flows with `afx_bank_flow_upload`.  Players that offer seek
   attach the optional matching AFC sidecar with
   `afx_flow_seek_index_load_file` before activating the flow.
3. Build the game-owned DSP image with `<aicaflow/dsp.h>`, upload it with
   `afx_dsp_scene_program`, and call `afx_update` regularly from the audio
   thread.
4. Before a level allocates its heap, prepare its music control flow and scene
   SFX bank. Stop instances, recycle them, then release old scene banks when
   the level ends.

DKR's integration is implemented in `dreamcast/audio_aicaflow.c`. Its paths
are relative to the selected asset mount: resident banks, scene banks, and
bank-bound music controls all live below `build/dc/aicaflow/`.

Do not write AICA registers or IPC messages directly beside this API. The host
library owns firmware compatibility, DMA, AICA RAM allocation and command
ordering.
