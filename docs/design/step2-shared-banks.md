# Step 2 design: shared banks and a planned AICA RAM layout

Status: approved on 2026-11-05 with four banks per flow and two PRs. Part A
is PR 2a (firmware ABI 13), which stacks on #16. Part B follows as PR 2b. The
AICAforge side (`--shared`, `afx_scene`) is a separate AICAforge PR.

## What the ports hit

- A flow binds exactly one bank. SM64 therefore copies 16–210 KB of core
  samples into every scene bank, and DKR re-uploads instruments with every song.
- The arena fragments. SM64's THI failed with 309 KB free when the largest free
  block was 157 KB. DKR restarts a menu song to defragment.
- SM64 (SMPK) and DKR (`build_aicaflow_budget.py`) each built their own scene
  plan. aica-a0 wants names for a Music Test row.

The design has two parts. Part A lets a flow play samples from up to four
banks, and it needs firmware ABI 13. Part B adds a scene file whose AICA layout
is fixed at build time, and a tool that plans that layout and checks that it
fits. Part B changes only the SH4 side and the tools.

## A. A flow plays from up to four banks

### Where the slot goes

A setup's sample address has 23 bits: CONTROL bits 0–6 and SAMPLE_LOW. The
address is relative to the bank. AICA RAM is 2 MiB, so bits 21–22 are zero in
every valid v7 flow. Those two bits become the **bank slot**, 0–3. A v7 flow
uses slot 0 everywhere and plays exactly as it does now.

The slot is stored wherever the address is stored:

- setups
- NOTE values that set CONTROL
- AFC checkpoint records
- the relocation's `sample_offset`, which must equal the setup address, as it
  must today

A relocation record stays 12 bytes.

### File

- There is a new flag, `AFX_FLAG_BANKS` (64).
- The header field `reserved1` becomes `banks_offset`, the file-relative offset
  of the bank list. The list is a u32 count (1–3) followed by the 8-byte
  identities of slots 1..count. Slot 0 is still `bank_id_low/high`. A flow
  without the flag keeps `banks_offset` at zero.
- With the flag, the image ends with a 16-byte **bank table** at the first
  4-byte boundary after the stream. The table has four entries, each a u16
  base and a u16 size, both in 32-byte units. The file carries zeros, and the
  SH4 writes the table after it uploads the image. Entry 0 stays zero, because
  slot 0 comes from the activation, as it does now.
- The AFX file version stays 7, as it did for LOOP: older validators reject
  both the flag and a nonzero `reserved1`.

### Firmware (ABI 13)

- Activation accepts `AFX_FLAG_BANKS`. It checks that the table lies inside the
  image and that every nonzero entry lies inside the asset arena. If either
  check fails, activation returns `AFX_BAD_COMMAND`, as it does today.
- `resolve_sample_address` takes the slot from bits 21–22.
  - Slot 0 uses the context, as it does now.
  - Slots 1–3 read their entry from the bank table. The table address follows
    from the context's stream end. The offset is then checked against that
    bank's size.
  - An empty slot, or an offset outside the bank, returns `AFX_BAD_SAMPLE`.
- The cost is one 32-bit AICA read for each NOTE or restored channel that uses
  slots 1–3. The 64-byte context, private state and the IPC payloads stay as
  they are.
- ABI 12 firmware refuses a flagged flow at activation, because it does not
  know the flag.

### Host

- `afx_flow_upload(const afx_bank_t *const banks[], uint32_t count, const void *file, uint32_t size, afx_asset_t *out)`.
  `banks[i]` fills slot `i`, and it must match the identity the file gives for
  that slot. `afx_bank_flow_upload()` is the one-bank case and does not change.
- Each relocation's range is checked against the bank in its slot.
- The flow keeps a reference on each of its banks. `afx_asset_release()` and
  the free cascade cover all of them. Up to three more bank handles are stored
  in a table that only multi-bank flows use, so the 84-byte asset slot does not
  grow.

### Authoring (AICAforge)

`--shared core.afb` (repeatable, up to three) works as follows. When a sample's
encoded bytes already occur in a shared bank, the flow points at those bytes in
that bank's slot. Only the remaining samples go into the flow's own bank.
Samples are matched by content, not by name, so the option works regardless of
how a port names its samples.

## B. Scene files and a planned layout

### AFS: one file per scene

An AFS file (`AFS\0`) holds the banks and flows of one scene. It also holds
the layout of the single AICA block that contains them all.

- **Header:** magic, version, identity, block bytes, entry count, and the
  offsets of the entries and the names.
- **Entries:** 32 bytes each. An entry gives:
  - its kind: bank, flow, slot or DSP ring
  - its name
  - its offset and size in the file
  - its offset and size in the block
  - for a flow, what fills each bank slot: an entry in this scene, or the
    identity of a bank in another scene
- **Names:** one zero-terminated name per entry. A flow entry also carries its
  loop flag and its length in ticks, which is what a Music Test row shows.

### Loading

- `afx_scene_load(data, size, parents, parent_count, &scene)` makes one
  allocation of the block size and places every entry at its planned offset.
  Banks are uploaded by DMA, as today. Flows are uploaded by PIO until step 3.
- A flow's bank slot can name a bank in a parent scene by identity, for
  example the core bank in the permanent scene.
- `afx_scene_find(scene, "name", &asset)` returns a flow by name.
  `afx_scene_entry(scene, i, &info)` returns entry `i` with its name, so a
  Music Test can list the scene's flows.
- A **slot** entry is a reserved range for content loaded later.
  - `afx_scene_slot_load(scene, slot, bank, bank_size, flow, flow_size, &asset)`
    uploads a bank and a flow into that range, never through the free list.
  - The slot's previous content must be idle, by the same rule as
    `afx_asset_release()`. Otherwise the call returns `AFX_ASSET_REFERENCED`.
  - DKR's per-level song becomes a slot sized for the largest song, so DKR no
    longer needs to restart a song to defragment.
- A **DSP ring** entry reserves the 2 KiB-aligned ring at its planned place,
  and the DSP scene calls use that range instead of allocating one. The
  permanent scene thereby fixes the order that docs/memory.md asks for.
- `afx_scene_release(scene)` frees the block once no instance plays any of its
  flows and no loaded scene binds any of its banks.

### Fragmentation

Nothing moves inside a scene. The arena holds the permanent scene plus one
level scene. During a transition that loads the next scene before releasing
the last, it holds two. Every free therefore returns a whole block, and no
compaction is needed. Plain `afx_asset_*` and `afx_bank_*` loads keep working
alongside scenes.

### Build-time plan and fit check

- `afx_scene`, a C tool alongside the other AICAforge tools, reads a
  line-based manifest that lists:
  - the firmware image (the arena comes from `afx_firmware_info_t`, as in DKR's
    `arena_bytes`)
  - the permanent scene
  - the level scenes
  - slot sizes
  - the ring size
- It writes the `.afs` files and a report of each scene's bytes, and of the
  headroom left by the permanent scene plus each level scene. If the manifest
  declares overlapping transitions, the report adds the largest second scene.
- It exits non-zero when anything does not fit.
- It applies the runtime's rounding (32 bytes, 2 KiB for the ring) using the
  code in `driver/format`, so the build check and the loader agree to the byte.
- DKR's `build_aicaflow_budget.py` becomes a manifest of this kind.

## Compatibility

- v7 flows, AFB banks, AFC files and `afx_bank_flow_upload()` do not change, and
  they keep playing on ABI 13.
- A flow with `AFX_FLAG_BANKS` needs ABI 13 firmware and the matching host
  library.
- AFS is new and optional.

## Tests

**Host:**

- validator cases for the flag: a bad count, a slot outside the list, and a
  relocation outside its slot's bank
- `test_arm7` for `resolve_sample_address` on each slot
- `test_bank` for multi-bank references, scene placement, slot reuse and the
  release cascade

**Console:**

- one flow played from a core bank and a scene bank, with each channel's
  sample address read back from the registers
- scenes and a slot loaded and freed repeatedly, checking that the largest free
  block stays the same

## Questions

1. Are four banks per flow, the two spare address bits, enough? More slots
   would need a change to the setup format.
2. Do you want two PRs, 2a (format, ABI 13, host) and 2b (AFS and the tool), or
   one? 2b has no firmware change.
3. The AICAforge side, `--shared` and `afx_scene`, lives in another repository.
   Should I open that PR, or should someone else?
