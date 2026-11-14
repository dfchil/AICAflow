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
activate the resulting flow. AICAflow takes the firmware's status reports by
itself and frees instances that end. The complete checked-in [quickstart](../examples/quickstart/code/main.c)
demonstrates memory inputs and completion waits; these are the corresponding
application steps:

1. Initialize once. Startup/shutdown require other host calls to be quiescent.
   `afx_init()` takes the AICA's interrupt (KOS event `ASIC_EVT_SPU_IRQ`) for
   the firmware's status reports, so the program must not hook it, and fails
   with `AFX_TIMEOUT` if the interrupt does not arrive (an emulator without it).
2. If using delay-memory DSP, choose/install its ring before filling the arena.
3. Load AFB with `afx_bank_load_file()` or `afx_bank_load_memory()` into a
   zero-initialized `afx_bank_t`.
4. Read the AFX bytes and call `afx_bank_flow_upload()` against that bank.
   Attach an optional AFC with `afx_flow_seek_index_load_file()` **before**
   activating; an active referenced flow cannot replace its seek index.
5. Activate with `afx_instance_activate()`. Apply player gain/tempo and other
   live controls using the returned instance handle.
6. The firmware reports every change of state by interrupt, and AICAflow's
   event thread takes it; `afx_instance_status()` shows it, without reading
   AICA RAM. A zero return from a lifecycle call means queued, not "ARM7
   finished": the result arrives later in the status.
7. An instance that ends, by itself or after `afx_instance_stop()`, is
   recycled by that thread: its slot is freed and its handle goes stale. Its
   channels are free sooner, as soon as the STOP is queued or the end is
   reported, so an activation right after a stop can have them;
   `afx_instance_replace()` stops one instance and activates a flow in its
   place, or, if it cannot, leaves the old one playing. STOP and PAUSE never
   return `AFX_BUSY` for an earlier command: the program need not retry them. `afx_set_end_callback()` reports each one with the status it ended
   with, from the event thread. The callback must not wait for a lock that the
   program holds while waiting for an instance to end: the event thread would
   stop. Hand the report to the program's own thread instead, as DKR does with a
   small queue. Once the handle is stale, its flow can be freed and its bank
   released; `afx_asset_release()` frees them as soon as nothing uses them,
   without waiting.

Integer APIs generally return zero on success and a negative `afx_result_t`
on failure. Handle/address getters use zero for invalid results. Do not spin
through allocation/queue failures: check memory, pending lifecycle
state and the observed ARM7 result. Preserve successfully
created handles until their normal cleanup has completed, including on partial
startup failure. `afx_shutdown()` requires flows, instances and banks released.

Load banks from one thread at a time. Other threads may make other AICAflow
calls while `afx_bank_load_file()` runs; it waits out the driver lock they take.

## AFX validation

The SH4 library defaults to trusted assets (`AFX_VALIDATE_ASSETS=0`): it checks
file layout, offsets, relocations, lanes and bank binding, but
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

An uploaded flow retains its banks; an instance retains its flow. Several
different or concurrent flows can share a bank. A flow with `AFX_FLAG_BANKS`
plays from up to four: upload it with `afx_flow_upload(banks, count, ...)`,
where `banks[i]` is the bank of slot `i` and `count` is
`afx_file_bank_count()` of the file. A level song can then take its
instruments from a resident core bank and the rest from a scene bank, with no
sample in both. A scene transition should stop old instances and wait for their handles to
go stale before freeing their flows/bank. Shared banks can stay resident across scene changes. The application
controls residency; the driver has no automatic sample cache.

`afx_instance_patch()` addresses a **local channel**, not a physical AICA
channel. Its mask and values are the existing register fields in ascending
field order. Pitch, mix, envelope, pan/filter, LFO and DSP send can be changed
while a voice is running/parked. Host-built SFX protect their immutable sample
binding and reject sample-address/loop changes and raw seek/rebuild. Use a
new bound flow when replacing that sample.

Runtime lane modifiers are persistent SH4-controlled grouping modifiers
(gain/mute/pan/send/pitch) on authored lane maps. These are distinct from ordinary
timed PATCH operations in the control stream.

## Scenes

An AFS scene file (see [Runtime asset formats](../driver/format/docs/assets.md#afs--scene))
holds the banks and flows of one scene with a layout the authoring tool
planned. `afx_scene_load(data, size, parents, count, &scene)` in
`<aicaflow/scene.h>` allocates one block of the scene's size and places every
bank and flow at its offset, banks by DMA. A flow's bank slots take the banks
with their identities in the scene, or else in the parents, typically a
permanent scene that holds the core bank. A failure loads nothing.

```c
afx_scene_t core, level;
afx_asset_t song;
afx_scene_load(core_afs, core_bytes, NULL, 0, &core);
afx_scene_load(level_afs, level_bytes, &core, 1, &level);
afx_scene_find(level, "theme", &song); /* a flow, by name */
```

`afx_scene_entry(scene, i, &info)` lists the entries with their names, kinds,
addresses and, for flows, the authored length and whether they loop: enough
for a Music Test row. A **slot** is a range reserved for content loaded later:
`afx_scene_slot_load(scene, "song", bank, bank_bytes, flow, flow_bytes, &asset)`
puts a bank and a flow there, never through the free list. What the slot held
must be idle, by the rule `afx_asset_release()` frees by; otherwise the call
returns `AFX_ASSET_REFERENCED` and the slot keeps it. A per-level song sized
for the largest one no longer fragments the arena.

`afx_scene_release(scene)` gives a scene up. Its flows and banks are released
as by `afx_asset_release()`, and the block is freed once no instance plays its
flows and no flow of another scene binds its banks. Until then a scene's
assets belong to it: `afx_asset_free()` and `afx_asset_release()` refuse them.
A scene's **ring** entry is the DSP ring the DSP scene calls use while the
scene is loaded and the ring is large enough; such a scene cannot be released
until the DSP is disabled. Plain bank and flow uploads keep working beside
scenes.

## Looping flows

A flow whose stream ends with `LOOP` (header flag `AFX_FLAG_LOOP`;
`afx_asset_loops()` says so and gives the authored tick it continues at)
repeats by itself, gaplessly, until stopped: it never ends on its own, so the
end callback does not see it until `afx_instance_stop()`. Each loop is
reported like other news, and `afx_set_loop_callback()` is called for it, with
the loop target as the status position (image-relative) and the hardware tick
the flow continued at as `next_deadline`; `afx_instance_status()` counts them in
`loops`. Seek ticks address the authored first pass.

A flow can also store a section that plays several times in a row once,
followed by `REPEAT` (header flag `AFX_FLAG_REPEAT`, firmware ABI 14). A song
of intro, A, A, B that loops back into A takes A's bytes once; the firmware
plays the span again, gaplessly, and repeats it again when the loop comes back
into it. Such a flow cannot be seeked (`AFX_UNSUPPORTED`).

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
