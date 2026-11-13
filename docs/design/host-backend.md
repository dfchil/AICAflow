# Host backend design: AICAflow on PC and in the browser

Status: built on 2026-11-13; [As built](#as-built) lists where it differs
from the proposal. Daniel asked for an AICAflow backend for
enDjinn's pc (Vulkan) and web targets, so that the PC builds of DKR, SM64 and
the examples play the same AFB, AFX and AFS files as the console.

## What the host must do

On the Dreamcast, three parts make a sound:

1. The SH4 library (`driver/sh4`) loads assets into AICA RAM and queues
   commands.
2. The ARM7 firmware (`firmware/aicaflow.drv`) runs the flows and writes the
   AICA's channel registers.
3. The AICA plays 64 voices and runs its DSP.

The host backend keeps parts 1 and 2 unchanged and replaces part 3 with a
software AICA. Programs call the same API and get the same timing, the same
command results and the same register writes as on hardware.

## Decisions

### The real firmware image runs on an ARM7 interpreter

The firmware is not compiled for the host. It reaches the AICA through fixed
addresses, and several register writes act at once: KYONEX keys channels on
and off, MCIPD raises the SH4 interrupt, and the timer and its reset drive the
FIQ. An interpreter sees each write as it happens, as the chip does, and needs
no host-only fork of `driver.c`. Flycast already runs this image.

The firmware targets `-mcpu=arm7di`, so the interpreter needs the ARM
instruction set without Thumb: data processing, multiply, single and block
transfers, SWP, MRS/MSR, branches and the FIQ. Coprocessor instructions and
SWI take the undefined and SWI vectors.

The interpreter runs a fixed number of instructions per output sample,
`AFX_HOST_ARM_CYCLES_PER_SAMPLE` (default 128, about 5.6 million a second).
The firmware's clock is Timer A, which counts samples, so flow timing does not
depend on this budget. Only lateness does, as on hardware.

### A software AICA

`driver/host/emu` models:

- AICA RAM (2 MiB) and the register file at its chip offsets;
- Timers A to C, SCIEB/SCIPD/SCIRE with SCILV levels and the FIQ, and
  MCIEB/MCIPD/MCIRE for the SH4 interrupt;
- 64 voices: 16-bit PCM, 8-bit PCM, Yamaha ADPCM (PCMS 2 and 3) and noise,
  with forward loops, LPSLNK, linear interpolation and OCT/FNS pitch;
- the amplitude envelope (AR, D1R, DL, D2R, RR, KRS), TL, VOFF, DISDL/DIPAN
  and the DSP send (ISEL/IMXL);
- the pitch and amplitude LFOs;
- the resonant low-pass filter with its envelope (FLV0 to FLV4, FAR to FRR, Q,
  LPOFF). It uses the state-variable model that AICAforge already assumes when
  it encodes a cutoff, a coefficient of 2·sin(π·f/44100);
- the DSP, ported from MAME's `aicadsp.cpp` (BSD-3-Clause), with its ring in
  AICA RAM at RBP;
- the mixer: direct outputs, EFREG through EFSDL/EFPAN, and MVOL, at 44.1 kHz.

MAME's `aica.cpp` (BSD-3-Clause) is the reference for the voice engine, the
envelope rate tables and the LFO tables. MAME does not implement the filter.
Code taken from MAME keeps its notice. Flycast is GPL, so nothing is copied
from it.

### The SH4 library builds for the host with private shims

`driver/sh4/src` compiles unchanged against shim headers in
`driver/host/shim`. They supply `dc/spu.h`, `dc/g2bus.h`, `dc/asic.h`,
`kos/*.h` and `arch/cache.h` and rename every KOS function to an
`afx_host_` function. The library therefore defines no KOS symbol, and it
links next to enDjinn's KOS shim without a clash.

`driver/host/src/platform.c` implements the shims on the software AICA:

| Dreamcast | Host |
|---|---|
| `g2_read_32`/`g2_write_32` on `0xa0700000` and `0xa0800000` | register file and RAM under the bus lock |
| `spu_memload`, `spu_memset` | `memcpy`/`memset` into AICA RAM |
| `spu_dma_transfer` | `memcpy`, then the callback, before it returns |
| `spu_disable`/`spu_enable` | hold or release the ARM7 reset; disable also keys every channel off |
| `asic_evt_request_threaded_handler` | a worker thread, or a direct call without threads |
| `thd_pass`, `thd_sleep` | yield or sleep, and run the ARM7 for a while |
| `timer_ms_gettime64` | the monotonic clock |

### Who advances the emulator

The AICA keeps time whether or not anyone listens:

- **Rendering.** `afx_host_audio_render()` produces samples and runs the ARM7,
  the timers, the voices and the DSP sample by sample. While a renderer runs
  (it rendered in the last 50 ms), only rendering advances time.
- **The wall clock.** Otherwise, each SH4-side access first advances the chip
  by the time that has passed, at most 100 ms at once, and discards the sound.
  A program with no audio device, or one blocked in a loop that polls
  `afx_status_timer_ticks()`, still sees its flows advance.
- **Waits.** `thd_pass` and `thd_sleep` inside the library always run the ARM7
  for a few hundred instructions, so a wait that cannot sleep (a browser)
  still sees the firmware acknowledge `afx_dsp_scene_program()`.

Tests render explicitly and so control time exactly.

### Threads

One bus lock guards the chip. The renderer takes it for at most 32 samples at
a time (0.7 ms), so an SH4-side access waits no longer than that. Every
SH4-side access takes it.

The SH4 interrupt follows KOS's threaded handler: the library's mask hook
runs as the line rises, and a worker thread runs its handler outside the bus
lock and then unmasks. A build without threads (Emscripten by default) has no
worker. Its handler runs where a KOS worker could have run: at the end of a
render, at a library wait, and whenever a library call takes the library's
lock from outside, before it does. The handler therefore never runs inside a
library call. JavaScript runs one event at a time, so nothing else
interrupts library code there.

### The audio API

```c
#include <aicaflow/host_audio.h>
uint32_t afx_host_audio_rate(void);                       /* 44100 */
void afx_host_audio_render(int16_t *stereo, uint32_t frames);
```

Any thread may call `afx_host_audio_render`; while it runs, it is the clock.
enDjinn's
host audio gets a generic source hook, `enj_host_audio_source_set()`, which
mixes one more source into its SDL2 callback. A small glue object in AICAflow
attaches the renderer to it when `afx_init()` succeeds. The pc and web targets
share that code, since the web build uses Emscripten's SDL2.

## Builds

- `make -C driver/host` builds `libaicaflow_pc.a` and `afx_render`.
- `make -C driver/host web` builds `libaicaflow_web.a` with `emcc`.
- `driver/host/host.mk` is for enDjinn applications. Included before
  `base_link.mk`, it adds the library, the include paths and the glue for
  `ENJ_TARGET=pc-endjinn` or `web-endjinn`, and does nothing for the
  Dreamcast. The quickstart and dynamic SFX examples use it.
- `make check` (and `make -C driver/host check`) builds and runs the host
  tests; `make -C driver/host web-check` runs them under Node.

## Tests

- `test_arm7`: the interpreter against hand-written instruction cases,
  including modes, banked registers and the FIQ.
- `test_aica`: voices programmed through the registers. Pitch over three
  octaves and a fractional FNS, 8-bit PCM, pan, a one-shot's end, release
  time against MAME's table, ADPCM sample by sample against AICAforge's
  decoder, and a DSP delay's echo to the sample.
- `test_host`: the real firmware and SH4 library play the loop fixture
  through the public API.
- The console checks, unchanged: `check_loop`, `check_commands`,
  `check_banks`, `check_scenes` and `check_timer`. They cover gapless loops,
  STOP behind ACTIVATE, replace, release when idle, held PATCH read back from
  the channel registers, banks, scenes with the DSP ring, and a 1002 Hz
  Timer A.

None of these prove that the host sounds like the console. A clean
audio-only capture from the console is the reference for that, compared with
the same flow rendered on the host.

## As built

- **Clock.** The proposal let only rendering advance time. A loop that polls
  the timer, such as `check_commands`, then never ended without a renderer,
  so the chip now keeps wall-clock time when nothing renders.
- **Single-threaded delivery.** The interrupt also runs when a library call
  takes the library's lock. Without that, the dynamic SFX example waited
  forever in a browser for an instance to park.
- **enDjinn.** enDjinn opens its audio device when the source attaches.
  Programs that use AICAflow set `flags.no_snd_init`, so no device was open.
  It also gains `thd_pass`, `thd_sleep`, `timer_us_gettime64` and
  `timer_ms_gettime64` in its host KOS shim, which the examples use.
- **Tools.** `afx_render` plays a flow and bank into a WAV, optionally with
  the room reverb, for listening without a console.
- **Tests.** The console checks (`driver/tests/check_*.c`) run unchanged on
  the host, with a test `kos.h` whose sleep renders. `make -C driver/host
  web-check` runs them under Node from an Emscripten build without threads.
  The checks use `#embed`; `tests/expand_embed.py` expands it for compilers
  without it, such as CI's clang 18.
- **Level.** Unity gain, as Flycast has it. DKR's music at its in-game gain
  (96/255) renders at about −16 dBFS RMS, between the console captures of its
  race (−12 dBFS) and title (−18 dBFS) music.

## Known limits

- The filter follows AICAforge's model, which has not been checked against
  hardware.
- MAME's envelope tables give the timing. The attack's shape is
  exponential, as on Yamaha's other chips; its exact curve is unverified.
- A KYONEX on a channel that is already keyed on does not retrigger it, as in
  MAME and Flycast.
- No AICA DMA, MIDI or EXTS inputs. The firmware uses none of them.
