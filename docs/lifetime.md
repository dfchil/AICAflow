# Lifetime

[Documentation](README.md)

An AFB owns one contiguous AICA sample allocation. An uploaded AFX flow retains
its bank, or each of its banks with `AFX_FLAG_BANKS`; an active instance
retains its flow. Release in this order:

1. Stop an active instance (one that ends by itself needs no stop).
2. Wait until its handle is stale: AICAflow recycles an instance that ended,
   and reports it to the end callback (`afx_set_end_callback()`).
3. Free the AFX flow with `afx_asset_free()`.
4. Release the bank with `afx_bank_release()`.

Or hand both to `afx_asset_release()` at any time: it frees an asset at once if
nothing uses it, and otherwise as soon as nothing does, a flow when its last
instance is recycled and a bank when the last flow that retains it is freed.
The program must not use a handle it released.

A scene (`<aicaflow/scene.h>`) owns its flows and banks: release the scene,
not its assets. `afx_scene_release()` releases each of them as above and frees
the scene's block with the last of them, so a level scene released while its
song plays, or while a later scene's flow binds its bank, stays in AICA RAM
until that ends.

Do not release a bank while a flow or instance refers to it. A scene can keep a
bank resident and upload/release many small AFX control streams against it.
Finite sounds must author `KEYOFF` and `END`; `PARK` is only for a controlled
flow that the SH4 will explicitly stop.
