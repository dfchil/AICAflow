# SH4 integration

[Documentation](README.md)

There is one runtime path: load a sample bank, bind a control flow, then create
instances. The driver library does not depend on enDjinn. Link
`driver/sh4/libaicaflow_host.a`, add `driver/include` and `driver/sh4/include`
to the include path, and provide the checked-in firmware.

Include `<aicaflow/host.h>` and `<aicaflow/bank.h>`, embed
`firmware/aicaflow.drv`, then initialize AICAflow once:

```c
alignas(32) static const unsigned char firmware[] = {
#embed "firmware/aicaflow.drv"
};

int result = afx_init(firmware, sizeof(firmware));
/* Check result before loading or activating assets. */
```

Load one AFB into an `afx_bank_t`, upload an AFX bound to that bank, then
activate the resulting flow. Call `afx_update()` regularly and recycle finished
instances. The complete checked-in [quickstart](../examples/quickstart/code/main.c)
demonstrates memory inputs and completion waits; these are the corresponding
application steps:

1. Initialize once. Startup/shutdown require other host calls to be quiescent.
2. If using delay-memory DSP, choose/install its ring before filling the arena.
3. Load AFB with `afx_bank_load_file()` or `afx_bank_load_memory()` into a
   zero-initialized `afx_bank_t`.
4. Read the AFX bytes and call `afx_bank_flow_upload()` against that bank.
   Attach an optional AFC with `afx_flow_seek_index_load_file()` **before**
   activating; an active referenced flow cannot replace its seek index.
5. Activate with `afx_instance_activate()`. Apply player gain/tempo and other
   live controls using the returned instance handle.
6. Call `afx_update()` and inspect `afx_instance_status()` regularly. A zero
   return from a lifecycle call means queued, not "ARM7 finished". Check both
   the API result and the later observed state/result.
7. On completion or explicit STOP, wait for DONE/ERROR, queue recycle and wait
   until the old handle is stale. Only then free its flow and release its bank.

Integer APIs generally return zero on success and a negative `afx_result_t`
on failure. Handle/address getters use zero for invalid results. Do not spin
through allocation/queue failures: check memory, pending lifecycle
state, execution budget and the observed ARM7 result. Preserve successfully
created handles until their normal cleanup has completed, including on partial
startup failure. `afx_shutdown()` requires flows, instances and banks released.

## AFX validation

The SH4 library defaults to trusted assets (`AFX_VALIDATE_ASSETS=0`): it checks
file layout, offsets, relocations, lanes, bank binding and resource limits, but
does not decode the event stream or recompute its work profile. Assets must be
validated offline and contain a nonzero authored work profile. Missing profiles
are rejected with `AFX_BAD_FORMAT`; they are not treated as zero work.

For untrusted assets or development, enable full stream validation and work-profile
verification. Rebuild the library when changing modes, then relink applications:

```sh
make -C driver/sh4 clean
make -C driver/sh4 AFX_VALIDATE_ASSETS=1
```

Use `AFX_VALIDATE_ASSETS=0` to return to trusted mode. Full validation also supports
older assets without an authored profile by calculating it during loading.

## Omitting KOS sound and its embedded firmware

For an AICAflow-only application, bypass KOS sound startup and shutdown by
adding this C file to the application's compiled sources:

```c
/* no_kos_audio.c */
int __wrap_snd_init(void) { return 0; }
void __wrap_snd_shutdown(void) {}
```

Pass these options to the final `kos-cc` link:

```sh
-Wl,--wrap=snd_init,--wrap=snd_shutdown,--gc-sections
```

For enDjinn builds, add them through `ENJ_LDFLAGS`. Other makefiles may use
`LDFLAGS`; confirm that the options appear in the final link command. Link the
wrapper object directly with the application objects. KOS must be built with
function/data sections so unused sound code and firmware can be discarded.

This wraps only `snd_init()` and `snd_shutdown()`, not every `snd_*` API.
Do not use KOS SFX/stream playback or enDjinn sound helpers in this configuration;
they require the bypassed subsystem and can pull its code back into the link.
Keep KOS's low-level SPU/G2/DMA support, which AICAflow uses, and retain the
normal `afx_init()`/`afx_shutdown()` lifecycle with AICAflow's own firmware.

Check the unstripped executable:

```sh
sh-elf-nm -S app.elf | grep -E 'snd_(init|shutdown|mem_|stream_drv_data)'
sh-elf-size app.elf
```

The two wrapper symbols should remain, but the original sound functions and
`snd_stream_drv_data` should be absent. The latter is KOS's embedded AICA
firmware. Symbol names may have leading underscores.

In an `-O2` music-player comparison, this removed 4,704 bytes of linked
sections, including 3,344 bytes of KOS firmware. Linker alignment reduced the
actual SH4 RAM saving to 3,520 bytes. Savings depend on the application and
KOS build. This configuration was link-tested, not hardware-tested.

## Shared banks and live controls

An uploaded flow retains its bank; an instance retains its flow. Several
different or concurrent flows can share a bank, but each flow binds to just
one. A scene transition should stop/recycle old instances before freeing their
flows/bank. Shared banks can stay resident across scene changes. The application
controls residency; the driver has no automatic sample cache.

`afx_instance_patch()` addresses a **local channel**, not a physical AICA
channel. Its mask and values are the existing register fields in ascending
field order. Pitch, mix, envelope, pan/filter, LFO and DSP send can be changed
while a voice is running/parked. Host-built SFX protect their immutable sample
binding and reject sample-address/loop changes and raw seek/rebuild. Use a
new bound flow when replacing that sample.

Runtime lane modifiers are persistent SH4-controlled grouping modifiers
(gain/mute/pan/send) on authored lane maps. These are distinct from ordinary
timed PATCH operations in the control stream.

## Seek, DSP and player metadata

Seeking requires a matching AFC and a paused music instance. SH4 selects the
checkpoint at or before the requested tick and submits its state with REBUILD;
ARM7 resolves sample addresses. No between-checkpoint replay occurs.
`afx_instance_seek_checkpoint` also returns the selected tick for player clocks.
The checkpoint table
stays on SH4; only prepared state uses temporary AICA staging. This is not a
sample-phase/DSP-buffer snapshot.

One DSP program belongs to the loaded scene. AFX register words choose channel
sends; the application installs/gates the DSP program and sets instance tempo.
The runtime does not read authoring profiles or infer a preset or tempo from
an AFX filename. See [AICAforge authoring](https://github.com/dfchil/AICAforge/blob/main/docs/authoring.md)
for generating application metadata.

For host-backed file playback, map the asset directory as `/pc` with `-m`.
Keep the host server running and disable computer sleep while playback uses it.

See [lifetime.md](lifetime.md) for the ownership rules and
[Runtime asset formats](../driver/format/docs/assets.md) for the asset contract. See
[Runtime ABI](specs/runtime.md) for the SH-4/ARM7 boundary.
