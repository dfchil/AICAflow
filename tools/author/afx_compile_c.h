#ifndef AICAFLOW_COMPILE_C_H
#define AICAFLOW_COMPILE_C_H

#include <stdint.h>

#include <aicaflow/format.h>
#include <aicaflow/limits.h>

typedef struct {
    uint32_t start_tick, end_tick;
    uint8_t key, velocity;
    /* MIDI bank/program are resolved at NOTE-on. They are authoring data only:
     * the emitted AFX contains setup indices, never MIDI lookups. */
    uint8_t bank_msb, bank_lsb, program;
    /* Zero selects a zone by MIDI attributes. Nonzero is a one-based resolved
     * zone index used by importers which expand layered SoundFont notes. */
    uint16_t setup_index;
    /* Zero uses the compiler's ordinary MIDI-velocity curve. Importers may
     * lower a source instrument's velocity law to an exact AICA MIX word. */
    uint16_t mix;
    /* KEYOFF time. Zero means end_tick. A later end_tick reserves the AICA
     * channel through a release tail without adding a runtime command. */
    uint32_t release_tick;
    /* MIDI controller state sampled at NOTE-on. It is authoring input only:
     * SoundFont lowering turns it into ordinary NOTE pitch/MIX values.
     * `controllers_valid` keeps hand-authored C notes source-compatible. */
    uint64_t controller_state;
    /* The complete CC snapshot and pressure values are likewise offline-only.
     * SF2 pmod/imod graphs may reference any MIDI controller at NOTE-on; no
     * controller or SoundFont state reaches AICA. */
    uint8_t controllers[128], poly_pressure, channel_pressure, pitch_sensitivity;
    /* Source identity is retained only while authoring.  It lets deterministic
     * offline performance transforms remain stable when a score is rebuilt. */
    uint32_t source_id, source_track, source_order, source_tick;
    int16_t attenuation_offset_centibels;
} afx_c_note_t;

typedef struct {
    uint8_t *afb, *afx, *afc, *afv;
    uint32_t afb_bytes, afx_bytes, afc_bytes, afv_bytes;
} afx_c_output_t;

/* One explicit AICA sample. `loop_end` is inclusive, as in AICA's LEA
 * register. `frames` is decoded PCM frame count even for ADPCM; `bytes` is
 * the contiguous AFB payload size. A non-looping source still has valid loop
 * bounds but clears looping. */
typedef struct {
    const uint8_t *data;
    uint32_t bytes, frames;
    uint8_t format, root_key, loop;
    uint16_t loop_start, loop_end;
    int16_t tuning_cents;
    /* Zero means the normal 44.1 kHz AICA base rate. */
    uint32_t sample_rate;
} afx_c_sample_t;

typedef struct {
    afx_c_sample_t sample;
    uint8_t key_min, key_max, velocity_min, velocity_max;
    uint8_t bank_msb, bank_lsb, program;
    /* AICA DSP-send register byte: IMXL in the high nibble, ISEL in low. */
    uint8_t dsp_send;
    /* Offline importers can lower an instrument's static controls straight
     * into the setup template.  These are ordinary AICA register words, not
     * another runtime abstraction. */
    uint32_t setup_mask;
    uint16_t setup[AFX_FIELD_COUNT];
} afx_c_zone_t;

/* A resolved control-stream event for importers whose source has live register
 * writes (for example MultiPCM). `fields` is indexed by AFX_FIELD_*; only
 * bits selected by `mask` are emitted.  NOTE setup indices are zero-based. */
typedef struct {
    uint32_t tick, order, mask;
    uint16_t setup;
    uint8_t opcode, channel;
    uint16_t fields[AFX_FIELD_COUNT];
} afx_c_event_t;

/* Normalize source-specific NOTE events into a compact setup dictionary.
 * Input NOTE events must carry PITCH and MIX; the function returns owned
 * event/template arrays for the common emitter.  Non-NOTE events are copied.
 */
int afx_c_optimize_events(const afx_c_event_t *events, uint32_t count,
                          const afx_c_zone_t *zones, uint32_t zone_count,
                          afx_c_event_t **out_events, afx_c_zone_t **out_zones,
                          uint32_t *out_zone_count);

/* Apply a source's explicit NOTE cluster policy before common emission.
 * `cluster_limit` is commands per control tick (1..38); note lifetimes move
 * together by whole ticks, so no timing is silently shortened. */
int afx_c_schedule_notes(afx_c_note_t *notes, uint32_t count, uint8_t cluster_limit);

/* Allocate AICA voices for a chronologically sorted score.  Importers with
 * source-side automation use the returned channel map to lower live controls
 * into PATCH commands while retaining the common allocator. */
int afx_c_assign_channels(const afx_c_note_t *notes, uint32_t count,
                          uint8_t *out_channels, uint32_t *out_channel_count);

/* Every note must select exactly one key range. The AFB retains all samples
 * in source-zone order, 32-byte aligned, sharing identical data pointers and
 * byte lengths. Only the AFX setup dictionary is pruned/reordered by usage;
 * its relocations still refer to the stable bank layout used by AFI indices. */
int afx_c_compile_zones(const afx_c_note_t *notes, uint32_t count,
                        uint32_t tick_rate, const afx_c_zone_t *zones,
                        uint32_t zone_count, afx_c_output_t *out);

/* Assemble a bank-bound AFX from already-resolved NOTE, PATCH and KEYOFF
 * events. `duration_ticks` is the source's final control tick.
 * An optional final PARK at duration_ticks emits a controlled flow without
 * song-only seek/visual sidecars, including a NOTE/PARK at tick zero. */
int afx_c_compile_events(const afx_c_event_t *events, uint32_t count,
                         uint32_t duration_ticks, uint32_t tick_rate,
                         const afx_c_zone_t *zones, uint32_t zone_count,
                         afx_c_output_t *out);

/* Compile a resolved timeline against one explicit sample. All output sidecars
 * are allocated with the AFB/AFX pair and released by afx_c_output_free(). */
int afx_c_compile_sample(const afx_c_note_t *notes, uint32_t count,
                         uint32_t tick_rate, const afx_c_sample_t *sample,
                         afx_c_output_t *out);

/* Compile a resolved monophonic/polyphonic note timeline using the built-in
 * sine source. The caller owns out through afx_c_output_free(). */
int afx_c_compile_sine(const afx_c_note_t *notes, uint32_t count,
                       uint32_t tick_rate, afx_c_output_t *out);
/* Make a VIZ1 sidecar from the final AFX command stream.  This deliberately
 * follows emitted NOTE, PATCH and KEYOFF commands rather than a parallel MIDI
 * approximation, so it remains aligned with offline AFP transforms. */
int afx_c_visualize(const uint8_t *afx, uint32_t bytes,
                    uint8_t **out_visual, uint32_t *out_bytes);
void afx_c_output_free(afx_c_output_t *out);

#endif
