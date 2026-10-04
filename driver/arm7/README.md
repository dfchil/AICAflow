# ARM7 timed executor

[Documentation](../../docs/README.md)

The firmware owns Timer A, but its FIQ only increments the reserved clock
and reloads the timer. Bounded normal-context code drains IPC and executes due
stream operations. It advertises bootstrap, lifecycle and playback capabilities.

The linker places code, data/BSS, five banked stacks and shared control state.
SH4 reserves optional DSP delay memory from the asset arena. Startup copies
initialized data, clears BSS and fills stack watermarks before entering C.
The host validates the embedded layout manifest before resetting/uploading.

See [the runtime contract](../../docs/specs/runtime.md),
[IPC](../../docs/specs/ipc.md), [memory layout](../../docs/memory.md) and
[testing](../../docs/testing.md). The numeric ABI and layout are defined in
[`protocol.h`](../include/aicaflow/protocol.h).

Build after sourcing your KOS environment:

```sh
make -C driver/arm7
make -C driver layout
```

The layout check validates the built firmware manifest against the headers.
Hardware checks must also exercise timed playback, PATCH, PARK/STOP and
clean instance teardown; see [Testing](../../docs/testing.md).

Startup and FIQ enable use register-form `msr cpsr_cf` for compatibility with
[Flycast v2.6's ARM recompiler](https://github.com/flyinghead/flycast/blob/v2.6/core/hw/arm7/arm7_rec.cpp#L148-L188),
which ignores `msr cpsr_c` writes. FIQ enable preserves flags read by `mrs`.

A bounded check boots the actual firmware and verifies that both its clock
and heartbeat advance in three SH-4-timed intervals:

```sh
source /opt/toolchains/dc/kos/environ.sh
PATH="$PATH:/opt/toolchains/dc/bin" make -C driver timer-check
/Applications/Flycast.app/Contents/MacOS/Flycast -config config:Debug.SerialConsoleEnabled=yes,config:Dreamcast.AutoLoadState=no,config:Dreamcast.AutoSaveState=no,config:UseReios=yes driver/build/check_timer.cdi
```

Expect `AFX_TIMER PASS` in the serial output. This checks clock liveness,
not audio quality or hardware clock calibration.
