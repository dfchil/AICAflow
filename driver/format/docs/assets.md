# Runtime asset formats

[Documentation](../../../docs/README.md)

## One playback model

Every sampled flow is one **AFX** control flow bound to **AFB** sample banks:
one bank, or up to four with `AFX_FLAG_BANKS` (see
[Several banks](#several-banks)). Several AFX files may bind to the same AFB.

At runtime the SH-4 loads the AFB payload as one contiguous AICA allocation,
then validates and uploads AFX images that carry the bank's identity. The AFB
has no runtime sample name table or lookup index: each AFX setup already holds
the required bank-relative address and AICA register values.

| Extension | Role | Read by |
| --- | --- | --- |
| `.afb` | Sample bank; one contiguous encoded payload | SH-4 loader |
| `.afx` | Timed control stream and AICA setup templates | SH-4 loader, ARM7 executor |
| `.afc` | Optional seek checkpoints for one exact AFX/AFB pair | SH-4 only |
| `.afi` | Optional binary AFB sample catalog | SH4-side one-shot code |
| `.afs` | Optional scene: banks and flows with their planned AICA layout | SH-4 loader |

These are the public runtime formats. Binary files use little-endian fields
and fixed headers.

## AFB — sample bank

An AFB begins with exactly 32 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `AFB\0` |
| 4 | 4 | version (`1`) |
| 8 | 8 | bank identity: low then high 32-bit word |
| 16 | 4 | payload offset (`32`, 32-byte aligned) |
| 20 | 4 | payload bytes |
| 24 | 4 | total file bytes |
| 28 | 4 | reserved, zero |

The payload is the exact encoded sample block copied into one AICA asset.
Individual samples may use PCM16, PCM8 or AICA ADPCM in the same bank; their
format, loop points and offsets are already represented in the AFX setup
records. The runtime recomputes neither a hash nor a checksum over sample data;
it compares the precomputed identity in the AFB and AFX headers.

## AFX — control flow

An AFX begins with an 80-byte header (`AFX2`, file version `7`). The upload
image starts at a 32-byte-aligned file offset.

All fields are unsigned 32-bit little-endian words:

| Offset | Field | Meaning |
| ---: | --- | --- |
| 0 | magic | `AFX2` |
| 4 | abi | AFX file version `7`, not firmware IPC ABI |
| 8 | total_size | Complete file bytes |
| 12 | flags | Controlled/music/lane/loop flags defined in `format.h` |
| 16 | image_offset | File-relative start of the 32-byte-aligned upload image |
| 20 | image_size | Upload image bytes |
| 24 | stream_offset | Image-relative bytecode start |
| 28 | stream_size | Bytecode bytes, including final END/PARK |
| 32 | control_id | Nonzero identity of the exact control image |
| 36 | setup_count | Number of 36-byte register setups |
| 40, 44 | bank_id_low, bank_id_high | Identity of the bound AFB (bank slot 0) |
| 48 | relocations_offset | File-relative relocation-table start |
| 52 | relocation_count | Number of 12-byte relocation records |
| 56 | reserved | Zero (was `loop_tick` before format API 6) |
| 60 | banks_offset | With `AFX_FLAG_BANKS`, file-relative start of the bank list; otherwise zero |
| 64 | required_channels | Local voice count required by this flow |
| 68, 72 | tick_rate_num, tick_rate_den | Authored ticks per second = numerator / denominator |
| 76 | work_profile | Peak burst commands in high 16 bits, register writes in low 16 bits |

The file header and relocation table are SH4 loader data; only the resolved
image is stored in AICA RAM. Setups contain format, loop and address words
bound to the separate AFB.

Each relocation gives a setup-pair offset, a bank-relative sample offset and
its byte length. The loader checks it against the AFB payload and resolves the
address once. AFX contains no sample data or names.
Each 12-byte on-disk relocation is `pair_offset`, `sample_offset`,
`sample_bytes` (three u32 words). `pair_offset` is image-relative and points to
the setup CONTROL/SAMPLE_LOW pair; `sample_offset` is **AFB-payload-relative**,
not a sample index or a file offset. The validator in `driver/format/src/codec.c`
is authoritative for ranges, flags and work-profile constraints.

`control_id` identifies the exact control image and binds its AFC sidecar.

### Several banks

A flow with `AFX_FLAG_BANKS` (64) plays samples from up to four banks, so a
scene bank and a core bank can serve one flow without copying samples. The
bank-relative sample address has 23 bits, and AICA RAM needs only 21, so bits
21–22 select the **bank slot** and the bits below are the offset in that bank.
The slot travels with the address: in setups, in NOTE values that set CONTROL,
in AFC checkpoint states and in each relocation's `sample_offset`, which still
equals its setup's address. Without the flag, every address is in slot 0, as
in any earlier file.

- Slot 0 is the bank of `bank_id_low/high`.
- `banks_offset` points at the bank list, which lies with the relocations
  before the image: a u32 count of further slots (1 to 3), then a low and a
  high u32 identity word per slot. Every identity is nonzero.
- The image ends with a 16-byte bank table at the first 4-byte boundary after
  the stream (`AFX_BANK_TABLE_OFFSET`). The file carries zeros; the loader
  writes one u32 per slot, the bank's AICA address and its payload length
  rounded up to 32 bytes, both in 32-byte units (low and high half). Entry 0
  stays zero, since activation carries slot 0's bank.

The file version stays 7: older validators reject the flag and a nonzero
`banks_offset`. An AFC binds slot 0's identity; its `control_id` pins the
rest. Playing such a flow needs firmware ABI 13.

## AFC — seek sidecar

An AFC begins with exactly 32 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `AFC\0` |
| 4 | 4 | version (`1`) |
| 8 | 4 | AFX control identity |
| 12 | 8 | AFB identity: low then high word |
| 20 | 4 | payload offset (`32`) |
| 24 | 4 | payload bytes |
| 28 | 4 | total file bytes |

The payload is a checkpoint table used only by the SH-4 when a player offers
seek. Normal playback does not need AFC. SH4 validates the header and retains
only the checkpoint payload. The file is never uploaded as an AICA asset.
An AFC file-size display includes its 32-byte header; it is not AICA RAM usage.

### Checkpoint payload

The payload begins with four u32 words: magic `CKP1`, checkpoint version `1`,
entry count, and reserved zero. Entries follow consecutively, without a
secondary chunk container. Each starts with four u32 words:

| Entry-relative offset | Meaning |
| ---: | --- |
| 0 | Authored checkpoint tick |
| 4 | Image-relative stream position |
| 8 | Authored ticks remaining until the next due stream operation |
| 12 | Active channel-state count |

Each channel state is 40 bytes: u32 local-channel number, followed by the 18
u16 register fields in AFX field order. Sample addresses remain bank-relative
in the file. Checkpoint spacing is chosen by the authoring tool and measured
in authored ticks, not a fixed wall-clock interval.

SH4 selects the preceding checkpoint, replays ordinary events to the target,
resolves bank addresses and sends prepared register states via REBUILD. That
operation can use temporary AICA staging memory; it does **not** load the AFC
table into AICA or make ARM7 interpret it. Checkpoints describe register/timeline
state, not a saved PCM decoder cursor, sample waveform or DSP delay-ring image;
seeking is musical reconstruction, not bit-exact sample-phase restoration.

## AFS — scene

An AFS holds the banks and flows of one scene and says where each goes in the
one AICA block that holds them all. The authoring tool plans that layout; the
loader allocates the block once and places every entry at its offset, so
nothing in a scene moves and the block is freed whole.

An AFS begins with exactly 32 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `AFS\0` |
| 4 | 4 | version (`1`) |
| 8 | 8 | scene identity: low then high word, nonzero |
| 16 | 4 | total file bytes |
| 20 | 4 | block bytes, a multiple of 32 |
| 24 | 4 | entry count, at least 1 |
| 28 | 4 | reserved, zero |

The entries follow at offset 32, 32 bytes each:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | kind: `1` bank, `2` flow, `3` slot, `4` DSP ring |
| 4 | 4 | file offset of the entry's zero-terminated name, or zero for none |
| 8 | 4 | file offset of its AFB or AFX file; zero for a slot or ring |
| 12 | 4 | that file's bytes |
| 16 | 4 | block offset: 32-byte aligned, a ring's 2 KiB aligned |
| 20 | 4 | block bytes, a multiple of 32 |
| 24 | 4 | a flow's authored length in ticks; zero for other kinds |
| 28 | 4 | reserved, zero |

- A **bank** entry holds a whole AFB file. Its payload goes at the block
  offset, and its block bytes are at least the payload rounded up to 32.
  No two banks of a scene have the same identity.
- A **flow** entry holds an AFX file, whose image goes at the block offset.
  Each of its bank slots takes the bank with that identity in the scene, or
  else in one of the parent scenes the program names when it loads the scene.
- A **slot** entry reserves a range for a bank and a flow loaded later: the
  bank's payload at its start, the flow's image at the next 32-byte boundary.
- A **ring** entry reserves the DSP delay ring, 16 KiB << RBL (RBL 0–3), on a
  2 KiB boundary in AICA RAM; a scene with one is allocated on a 2 KiB
  boundary. A scene has at most one.
- Names come after the entries. A name is nonempty and unique in its scene.
  The files come after the names, at 32-byte boundaries so that a bank's
  payload can go to AICA by DMA straight from the loaded file.

`afx_scene_validate()` checks the layout, including that the block ranges lie
in the block and do not overlap. The AFB and AFX files are checked as they
load. A scene needs no firmware support beyond what its flows need.

## AFI — SH4 sample catalog

AFI is the optional binary catalog that lets SH4-side code start AFB samples
as direct one-shots without scanning an AFX setup dictionary. It is never
uploaded to AICA or interpreted by ARM7. Compact and named records are supported;
each catalog binds to one AFB.

An AFI begins with a fixed, 32-byte little-endian header:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `AFI\0` |
| 4 | 4 | version (`1`) |
| 8 | 8 | AFB identity: low then high word |
| 16 | 4 | record offset (`32`) |
| 20 | 4 | unique sample count |
| 24 | 4 | record bytes (`16` or `32`) |
| 28 | 4 | total file bytes, padded to 32 bytes |

Each compact 16-byte record is little-endian:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | sample **file offset** in the AFB |
| 4 | 4 | effective sample rate in Hz |
| 8 | 4 | decoded sample length in frames |
| 12 | 1 | AICA format (`0` PCM16, `1` PCM8, `2` ADPCM) |
| 13 | 3 | reserved, zero |

The 32-byte named record appends a zero-padded, fixed 16-byte source sample
name at offset 16. AFI describes direct one-shots; loop points, root key
and tuning are stored in AFX setups.

## AFM songs

An AFM file is a song: sections played in an order. Its 96-byte header
(`afx_song_header_t` in [format.h](../include/aicaflow/format.h)) is followed
by the relocations and bank list as in an AFX, then the 32-byte aligned image:
a 32-byte image header (`afx_song_image_header_t`), the setups, the lane map,
the section table (stream offset, stream size and body ticks per section),
the order (16-bit section indices) and the streams, with the bank table after
them when the song uses `AFX_FLAG_BANKS`. `afx_song_validate()` checks a file;
[the design note](../../../docs/design/song-sections.md) explains the layout.
