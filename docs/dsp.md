# DSP programming

[Documentation](README.md)

Use `<aicaflow/dsp.h>` to construct an `afx_dsp_program_t` in C. The helpers
encode one AICA DSP instruction at a time and validate the resource limits
before upload. Install a completed program with `afx_dsp_scene_program()` and
remove it with `afx_dsp_scene_disable()`.

## DSP cheat sheet

| Concept | Meaning when authoring a step |
| --- | --- |
| Voice send | A voice's `dsp_send` byte is `IMXL << 4 | ISEL`: the high nibble is send level, the low nibble selects `MIXS0..15`. Thus `0xf0` is a full send to `MIXS0`; `0xf1` is a full send to `MIXS1`. Set `DIRECT=0` to hear only the DSP return. |
| DSP input | `IRA` selects an input when `XSEL=1`. Use `AFX_DSP_INPUT_MIXS0`/`MIXS1` for voice buses; `AFX_DSP_INPUT_MEMS0` reads the first delay-memory latch. |
| Multiply | `YSEL=AFX_DSP_Y_COEF` selects the coefficient at the current step. `COEF` values are signed, aligned Q1.15-ish gains: 32760 is near unity, 8192 is about 1/4. |
| ACC timing | An instruction computes the next `ACC`, but `TWT`, `EWT` and `SHIFT` consume the previous `ACC`. Compute at one step; write or return at the next. |
| Short state | `TWT` writes `TEMP`; later steps read it with `TRA`. `TEMP` rotates one logical address per sample, so it is for short state, not long delays. |
| Delay RAM | `MRD`/`MWT` use `MADRS[MASA]` and must be on odd steps. MADRS indices are 0–63; do not double them. Read with `MRD`, latch with `IWT`, then read `MEMS`. Use one `NOFL` format throughout: `AFX_DSP_MEMORY_AICA_FLOAT` or `AFX_DSP_MEMORY_LINEAR`. |
| Stereo return | `EWT` sends the previous `ACC` to `EWA`: `AFX_DSP_RETURN_LEFT` is `EFREG0`; `AFX_DSP_RETURN_RIGHT` is `EFREG1`. |

Use the named constants above rather than their hardware numbers. The API header
has the full operand and ordering rules; `examples/multiple_dsp_effects` is a
complete two-input program that applies them.

DSP belongs to the loaded scene, not to an individual AFX flow. A flow selects
its existing AICA DSP-send register value; SH4 owns program installation and
return gating. Use `afx_dsp_scene_returns(false)` to audition dry routing
without replacing the program.

SH4 allocates DSP rings within the AICA asset arena, on 2 KiB boundaries.
A program without `MRD`/`MWT` instructions needs no ring; RBL 0, 1, 2 and 3
allocate 16, 32, 64 and 128 KiB respectively. Install a memory-using scene
before loading assets to avoid fragmentation. Replacement stops the old scene
before allocating its replacement; allocation failure leaves DSP off.
An acknowledged disable clears and frees the ring. A timeout keeps it allocated
until a later disable is acknowledged or the driver is shut down.

The exact placement of the ring relative to the asset arena, IPC state and
ARM7 stacks is shown in [Memory layout](memory.md). The runtime ownership
contract is in [Runtime ABI](specs/runtime.md).

For authored music, keep the scene preset and its DSP-send amount in the
offline `.afp` profile. The profile rewrite puts the send in the derived AFX;
the player installs the named scene before loading the matching AFB. Neither
the profile nor an allocator policy is interpreted by ARM7.
Read `afx_profile describe` during the offline build to record preset/send/
tempo in player metadata. Merely uploading the profiled AFX does not install
the program: it only supplies register send values. The standard
`afx_dsp_scene_program()` reserves 128 KiB for a memory-using program;
`afx_dsp_scene_program_ring()` selects an explicit supported RBL.

The enDjinn-based `examples/dsp_effects_player` lets a user audition the
built-in C presets with generated tones, an impulse and the CC0 Wilhelm-scream
input. `examples/multiple_dsp_effects` demonstrates two separate `MIXS` inputs
in one scene. The detailed operand, memory and scheduling notes live beside the
API as inline comments in `driver/sh4/include/aicaflow/dsp.h`.
