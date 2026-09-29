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

`Makefile.dc` uses the checked-in ARM7 firmware and builds the SH-4 static library, imports the
N64 music/SFX assets, and links `dkracing.elf`. `make -f Makefile.dc -j8 cdi`
uses the same generated audio files with the `/cd` mount.

## Runtime order

The public API is in `driver/sh4/include/aicaflow/host.h`; the bank loader API
is in `driver/sh4/include/aicaflow/sfx_bank.h`.

1. Embed `driver/arm7/aicaflow.drv`, then call `afx_init` once.
2. Load resident SFX with `afx_sfx_bank_load_file`; load shared music samples
   with `afx_sfx_bank_load_samples_file` and one AFC1 control stream per song.
3. Call `afx_dsp_scene_enable` after initialisation and `afx_update` regularly
   from the audio thread.
4. Before a level allocates its heap, prepare its music control flow and scene
   SFX bank. Stop instances, recycle them, then release old scene banks when
   the level ends.

DKR's integration is implemented in `dreamcast/audio_aicaflow.c`. Its paths
are relative to the selected asset mount: resident banks, scene banks,
fallback banks and music controls all live below `build/dc/aicaflow/`.

Do not write AICA registers or IPC messages directly beside this API. The host
library owns firmware compatibility, DMA, AICA RAM allocation and command
ordering.
