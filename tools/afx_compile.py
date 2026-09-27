#!/usr/bin/env python3
"""Compile a resolved MIDI performance into one self-contained ABI-6 AFX.

The bring-up path supports an explicitly mapped looped sine and an explicit
single-zone SoundFont PCM16 path. The latter requires an author-selected
channel and resample rate; it is a deterministic lowering seam, not yet a
complete piano/cello renderer. Unsupported source features remain candidate
diagnostics; malformed source performance is rejected before an AFX image is
written.
"""

import argparse
import hashlib
import json
import math
import struct
import sys
from fractions import Fraction
from io import BytesIO
from pathlib import Path


ROOT = Path(__file__).resolve().parent

import afx_adpcm
import afx_midi
import afx_sf2

AFX_FILE_MAGIC = 0x32584641
AFX_ABI_VERSION = 6
AFX_FILE_HEADER_BYTES = 80
AFX_FIELD_PITCH = 6
AFX_FIELD_LFO = 7
AFX_FIELD_DSP_SEND = 8
AFX_FIELD_DIRECT = 9
AFX_FIELD_TOTAL_LEVEL = 10
AFX_OP_END = 0
AFX_OP_WAIT8 = 1
AFX_OP_WAIT16 = 2
AFX_OP_WAIT32 = 3
AFX_OP_NOTE = 0x10
AFX_OP_NOTE_PL = 0x14
AFX_OP_PATCH = 0x11
AFX_OP_PATCH_LEVEL = 0x15
AFX_OP_KEYOFF = 0x12
AFX_FLAG_MUSIC = 2
AFX_FLAG_MUSIC_CHORUS = 8
AFX_FLAG_LANES = 16
SETUP_BYTES = 36
PCM16 = 0
PCM8 = 1
ADPCM = 2
AICA_NATIVE_SAMPLE_RATE = 44100
# ABI-6's cross-built firmware asset base is 0x2a40; assets end where DSP RAM
# begins. A final AFX must fit this resident arena before SH-4 upload.
AICA_ASSET_CAPACITY = 0x1DC000 - 0x2b80
AFX_CHECKPOINT_MAGIC = 0x31504B43  # "CKP1" little-endian.
AFX_CHECKPOINT_VERSION = 1
CHECKPOINT_TICKS = 1000
AR_TIME_MS = (100000, 100000, 8100, 6900, 6000, 4800, 4000, 3400, 3000, 2400, 2000,
              1700, 1500, 1200, 1000, 860, 760, 600, 500, 430, 380, 300, 250, 220,
              190, 150, 130, 110, 95, 76, 63, 55, 47, 38, 31, 27, 24, 19, 15, 13,
              12, 9.4, 7.9, 6.8, 6, 4.7, 3.8, 3.4, 3, 2.4, 2, 1.8, 1.6, 1.3, 1.1,
              .93, .85, .65, .53, .44, .4, .35, 0, 0)
DR_TIME_MS = (100000, 100000, 118200, 101300, 88600, 70900, 59100, 50700, 44300,
              35500, 29600, 25300, 22200, 17700, 14800, 12700, 11100, 8900, 7400,
              6300, 5500, 4400, 3700, 3200, 2800, 2200, 1800, 1600, 1400, 1100, 920,
              790, 690, 550, 460, 390, 340, 270, 230, 200, 170, 140, 110, 98, 85, 68,
              57, 49, 43, 34, 28, 25, 22, 18, 14, 12, 11, 8.5, 7.1, 6.1, 5.4, 4.3,
              3.6, 3.1)


class CompileError(RuntimeError):
    pass


def require_asset_capacity(image_size: int) -> None:
    if image_size > AICA_ASSET_CAPACITY:
        raise CompileError(
            f"asset arena exhausted: image {image_size} bytes exceeds {AICA_ASSET_CAPACITY} "
            f"by {image_size - AICA_ASSET_CAPACITY}; choose lower resampling, fewer regions, "
            "a loop/wavetable approximation, or an approved compressed representation")


def align(value: int, multiple: int) -> int:
    return (value + multiple - 1) // multiple * multiple


def mapping_key(note: dict) -> str:
    return f"{note['bank_msb']}:{note['bank_lsb']}:{note['program']}"


def instrument_config(mapping: dict, note: dict) -> dict:
    """Resolve base → track → source instrument → original source note."""
    for field in ("instruments", "track_overrides", "source_overrides", "note_overrides"):
        if not isinstance(mapping.get(field, {}), dict):
            raise CompileError(f"{field} must be an object")
    base = mapping["instruments"][mapping_key(note)]
    if not isinstance(base, dict):
        raise CompileError(f"instrument {mapping_key(note)} must be an object")
    overrides = mapping.get("track_overrides", {})
    override = overrides.get(str(note["track"]), {})
    if not isinstance(override, dict):
        raise CompileError(f"track {note['track']}: override must be an object")
    source_override = mapping.get("source_overrides", {}).get(mapping_key(note), {})
    if not isinstance(source_override, dict):
        raise CompileError(f"instrument {mapping_key(note)}: override must be an object")
    note_override = mapping.get("note_overrides", {}).get(str(note.get("source_note_id", note["id"])), {})
    if not isinstance(note_override, dict):
        raise CompileError(f"note {note['id']}: override must be an object")
    return {**base, **override, **source_override, **note_override}


def resolve_mapping_paths(mapping: dict, base: Path) -> dict:
    for value in mapping.get("instruments", {}).values():
        if isinstance(value, dict) and value.get("kind") == "sf2_pcm16" and "soundfont" in value:
            path = Path(value["soundfont"])
            if not path.is_absolute(): value["soundfont"] = str((base / path).resolve())
    return mapping


def pitch(key: int, root_key: int, tuning_cents: int = 0) -> int:
    """AICA OCT/FNS packing, reusing the proven old converter's equation."""
    ratio = 2.0 ** ((key - root_key + tuning_cents / 100.0) / 12.0)
    octave = math.floor(math.log2(ratio))
    fraction = int(1024.0 * (ratio / (2.0 ** octave) - 1.0))
    return ((max(-8, min(7, octave)) & 0xF) << 11) | max(0, min(1023, fraction))


def aica_rate_tuning_cents(rate: int) -> int:
    return round(1200 * math.log2(rate / AICA_NATIVE_SAMPLE_RATE))


def midi_pitch_bend_cents(state: dict) -> float:
    bend, span = state.get("pitch_bend", 0), state.get("pitch_bend_range_cents", 200)
    if not isinstance(bend, int) or not -8192 <= bend <= 8191:
        raise CompileError("MIDI pitch bend must be in -8192..8191")
    if not isinstance(span, int) or not 0 <= span <= 12799:
        raise CompileError("MIDI pitch bend range must be in 0..12799 cents")
    return bend * span / 8192


def record_pitch(note: dict, record: dict, state: dict | None = None) -> int:
    state = note if state is None else state
    return pitch(note["key"], record["root_key"], record.get("tuning_cents", 0) +
                 record.get("rate_tuning_cents", 0) + midi_pitch_bend_cents(state))


def encode_wait(wait: int) -> bytes:
    if wait <= 0:
        return b""
    if wait <= 0xFF:
        return bytes((AFX_OP_WAIT8, wait))
    if wait <= 0xFFFF:
        return bytes((AFX_OP_WAIT16,)) + struct.pack("<H", wait)
    return bytes((AFX_OP_WAIT32,)) + struct.pack("<I", wait)


def encode_note(channel: int, setup: int, note_pitch: int, total_level: int,
                direct: int | None = None, dsp_send: int | None = None,
                lfo: int | None = None) -> bytes:
    if direct is None and dsp_send is None and lfo is None:
        return struct.pack("<BBHHH", AFX_OP_NOTE_PL, channel, setup, note_pitch, total_level)
    fields = {AFX_FIELD_PITCH: note_pitch, AFX_FIELD_TOTAL_LEVEL: total_level}
    if direct is not None: fields[AFX_FIELD_DIRECT] = direct
    if dsp_send is not None: fields[AFX_FIELD_DSP_SEND] = dsp_send
    if lfo is not None: fields[AFX_FIELD_LFO] = lfo
    return bytes((AFX_OP_NOTE, channel)) + struct.pack("<HI", setup, sum(1 << field for field in fields)) + \
        b"".join(struct.pack("<H", fields[field]) for field in sorted(fields))


def encode_patch(channel: int, fields: dict[int, int]) -> bytes:
    if set(fields) == {AFX_FIELD_TOTAL_LEVEL}:
        return struct.pack("<BBH", AFX_OP_PATCH_LEVEL, channel, fields[AFX_FIELD_TOTAL_LEVEL])
    mask = sum(1 << field for field in fields)
    return bytes((AFX_OP_PATCH, channel)) + struct.pack("<I", mask) + \
        b"".join(struct.pack("<H", fields[field]) for field in sorted(fields))


def build_stream(events: list[tuple], encode_event, end_tick: int | None = None, event_offsets: list | None = None) -> tuple[bytes, list[tuple], int]:
    """Encode same-tick groups once and retain the post-WAIT decoder positions."""
    stream, batches, previous = bytearray(), [], 0
    cursor = 0
    while cursor < len(events):
        when = events[cursor][0]
        stream += encode_wait(when - previous)
        start = len(stream)
        while cursor < len(events) and events[cursor][0] == when:
            if event_offsets is not None: event_offsets.append(len(stream))
            stream += encode_event(events[cursor])
            cursor += 1
        batches.append((when, start))
        previous = when
    if end_tick is not None and end_tick > previous:
        stream += encode_wait(end_tick - previous)
        batches.append((end_tick, len(stream)))
    stream += bytes((AFX_OP_END,))
    return bytes(stream), batches, len(stream) - 1


def source_event_map(timeline, events, offsets, stream_offset):
    notes = {note["id"]: note for note in timeline["notes"]}
    return [{"image_offset": stream_offset + offset, "tick": event[0],
             "kind": "note" if event[1] == 1 else "keyoff" if event[1] == 0 else "patch",
             "voice_id": event[3], "source_note_id": notes[event[3]].get("source_note_id", event[3]),
             "local_channel": event[2]} for event, offset in zip(events, offsets)]


def work_report(events: list[tuple]) -> dict:
    """Report decoder commands and concrete channel-register writes by source tick."""
    total_writes = peak_commands = peak_writes = peak_tick = command_peak_writes = 0
    cursor = 0
    while cursor < len(events):
        tick = events[cursor][0]
        commands = writes = 0
        while cursor < len(events) and events[cursor][0] == tick:
            _, kind, _, _, payload = events[cursor]
            commands += 1
            # NOTE writes control once to suppress key-on, 17 remaining fields,
            # then control again to execute. KEYOFF is one control write.
            writes += 19 if kind == 1 else (1 if kind == 0 else len(payload))
            cursor += 1
        total_writes += writes
        if commands > peak_commands or (commands == peak_commands and writes > command_peak_writes):
            peak_commands, command_peak_writes, peak_tick = commands, writes, tick
        peak_writes = max(peak_writes, writes)
    return {"decoded_commands": len(events), "expanded_register_writes": total_writes,
            "peak_tick": peak_tick, "peak_decoded_commands": peak_commands,
            "peak_expanded_register_writes": peak_writes}


def encode_work_profile(work: dict) -> int:
    commands, writes = work["peak_decoded_commands"], work["peak_expanded_register_writes"]
    if commands > 0xffff or writes > 0xffff:
        raise CompileError(f"execution budget exhausted: peak {commands} commands / {writes} writes exceeds header profile")
    return commands << 16 | writes


def allocate_control_channels(intervals: list[dict]) -> tuple[dict, list[int] | None]:
    lanes = {interval.get("lane") for interval in intervals}
    if lanes == {None} or not intervals:
        return afx_midi.allocate_local_channels(intervals), None
    if None in lanes or any(type(lane) is not int or not 0 <= lane < 64 for lane in lanes):
        raise CompileError("every note in a lane-aware flow needs a lane in 0..63")
    lane_map, offset, peak = [], 0, 0
    for lane in sorted(lanes):
        group = sorted((interval for interval in intervals if interval["lane"] == lane),
                       key=lambda interval: (interval["start_tick"], interval["end_tick"], interval["id"]))
        allocation = afx_midi.allocate_local_channels(group)
        for interval in group:
            interval["local_channel"] += offset
        count = allocation["local_channels"]
        lane_map.extend([lane] * count)
        offset += count
        peak += allocation["peak_overlap"]
    return {"local_channels": offset, "peak_overlap": peak}, lane_map


def recolor_control_intervals(timeline: dict, tick_rate: int) -> None:
    intervals = [{"start_tick": note.get("control_start_tick",
                                          tick_to_control_ticks(timeline, note["start_tick"], tick_rate)),
                  "end_tick": control_lifetime_end(timeline, note, tick_rate),
                  "id": note["id"], "lane": note.get("lane")}
                 for note in timeline["notes"]]
    timeline["allocation"], lane_map = allocate_control_channels(intervals)
    if lane_map is None:
        timeline.pop("lane_map", None)
    else:
        timeline["lane_map"] = lane_map
    for note, interval in zip(timeline["notes"], intervals):
        note["local_channel"] = interval["local_channel"]


def control_start(timeline: dict, note: dict, tick_rate: int) -> int:
    value = note.get("control_start_tick")
    return value if value is not None else tick_to_control_ticks(timeline, note["start_tick"], tick_rate)


def control_end(timeline: dict, note: dict, tick_rate: int) -> int:
    value = note.get("control_end_tick")
    return value if value is not None else tick_to_control_ticks(timeline, note["end_tick"], tick_rate)


def control_lifetime_end(timeline: dict, note: dict, tick_rate: int) -> int:
    return note.get("lifetime_end_tick", control_end(timeline, note, tick_rate))


def set_control_end(note: dict, tick: int) -> None:
    note["control_end_tick"] = tick
    if "tail_ticks" in note:
        note["lifetime_end_tick"] = tick + note["tail_ticks"]


def prepare_pcm16_tails(timeline: dict, mapping: dict, tick_rate: int) -> None:
    """Reserve explicitly authored PCM release tails before colouring voices."""
    for note in timeline["notes"]:
        config = instrument_config(mapping, note)
        tail_ms = config.get("tail_ms", 0)
        if isinstance(tail_ms, bool) or not isinstance(tail_ms, int) or not 0 <= tail_ms <= 10_000:
            raise CompileError(f"{mapping_key(note)}: tail_ms must be an integer in 0..10000")
        note["tail_ticks"] = round(tail_ms * tick_rate / 1000)
        note["lifetime_end_tick"] = control_end(timeline, note, tick_rate) + note["tail_ticks"]
    recolor_control_intervals(timeline, tick_rate)


def spread_keyoff_clusters(timeline: dict, mapping: dict, tick_rate: int) -> dict | None:
    """Apply an author-selected upper bound to same-tick releases.

    A later release cannot reuse the original local channel, so recolour the
    complete interval set after scheduling.  This is deliberately opt-in:
    exact MIDI release timing remains the default.
    """
    limit = mapping.get("keyoff_cluster_limit")
    if limit is None:
        return None
    if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 64:
        raise CompileError("keyoff_cluster_limit must be an integer in 1..64")
    original_channels = timeline["allocation"]["local_channels"]
    occupied, max_delay = {}, 0
    for note in sorted(timeline["notes"], key=lambda item:
                       (control_end(timeline, item, tick_rate), item["track"], item["order"], item["id"])):
        original = control_end(timeline, note, tick_rate)
        scheduled = max(original, control_start(timeline, note, tick_rate) + 1)
        while occupied.get(scheduled, 0) >= limit:
            scheduled += 1
        occupied[scheduled] = occupied.get(scheduled, 0) + 1
        set_control_end(note, scheduled)
        max_delay = max(max_delay, scheduled - original)
    recolor_control_intervals(timeline, tick_rate)
    return {"limit": limit, "max_delay_ticks": max_delay,
            "original_required_channels": original_channels,
            "required_channels": timeline["allocation"]["local_channels"]}


def spread_event_clusters(timeline: dict, mapping: dict, tick_rate: int) -> dict | None:
    """Bound every timed NOTE/KEYOFF group with an explicit author policy."""
    limit = mapping.get("event_cluster_limit")
    if limit is None:
        return None
    if mapping.get("keyoff_cluster_limit") is not None:
        raise CompileError("event_cluster_limit and keyoff_cluster_limit are mutually exclusive")
    if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 64:
        raise CompileError("event_cluster_limit must be an integer in 1..64")
    original_channels = timeline["allocation"]["local_channels"]
    events = []
    for note in timeline["notes"]:
        events.append((control_start(timeline, note, tick_rate), 1, note))
        events.append((control_end(timeline, note, tick_rate), 0, note))
    occupied, start_delay, keyoff_delay = {}, 0, 0
    for source_tick, kind, note in sorted(events, key=lambda item:
                                          (item[0], item[1], item[2]["track"],
                                           item[2]["order"], item[2]["id"])):
        scheduled = source_tick if kind else max(source_tick, control_start(timeline, note, tick_rate) + 1)
        while occupied.get(scheduled, 0) >= limit:
            scheduled += 1
        occupied[scheduled] = occupied.get(scheduled, 0) + 1
        if kind:
            note["control_start_tick"] = scheduled
            start_delay = max(start_delay, scheduled - source_tick)
        else:
            set_control_end(note, scheduled)
            keyoff_delay = max(keyoff_delay, scheduled - source_tick)
    recolor_control_intervals(timeline, tick_rate)
    return {"limit": limit, "max_note_start_delay_ticks": start_delay,
            "max_keyoff_delay_ticks": keyoff_delay,
            "original_required_channels": original_channels,
            "required_channels": timeline["allocation"]["local_channels"]}


def checkpoint_plan(events: list[tuple], batches: list[tuple], stream_offset: int,
                    end_offset: int, interval: int = CHECKPOINT_TICKS) -> list[dict]:
    """Snapshot active local voices after each whole-second source position."""
    if not events: return []
    plans, active = [], {}
    event_cursor = batch_cursor = 0
    last_tick = events[-1][0]
    for tick in range(0, last_tick + 1, interval):
        while event_cursor < len(events) and events[event_cursor][0] <= tick:
            _, kind, channel, _, payload = events[event_cursor]
            if kind == 0:
                active.pop(channel, None)
            elif kind == 1:
                active[channel] = {"note": payload[0], "record": payload[1], "patch": {}}
            elif channel in active:
                active[channel]["patch"].update(payload)
            event_cursor += 1
        while batch_cursor < len(batches) and batches[batch_cursor][0] <= tick:
            batch_cursor += 1
        if batch_cursor < len(batches):
            next_tick, after_wait = batches[batch_cursor]
            position, remaining = stream_offset + after_wait, next_tick - tick
        else:
            position, remaining = stream_offset + end_offset, 0
        plans.append({"tick": tick, "stream_position": position, "local_tick": remaining,
                      "active": sorted(active.items())})
    return plans


def encode_checkpoints(plan: list[dict], state_for_payload) -> bytes:
    out = bytearray(struct.pack("<4I", AFX_CHECKPOINT_MAGIC, AFX_CHECKPOINT_VERSION, len(plan), 0))
    for entry in plan:
        out += struct.pack("<4I", entry["tick"], entry["stream_position"], entry["local_tick"],
                           len(entry["active"]))
        for local, payload in entry["active"]:
            state = state_for_payload(payload)
            if len(state) != 18: raise CompileError("checkpoint state is not a complete AICA setup")
            out += struct.pack("<I18H", local, *state)
    return bytes(out)


def tick_to_control_ticks(timeline: dict, tick: int, rate: int) -> int:
    if tick < 0:
        raise CompileError("negative source tick")
    tempo, cursor, elapsed = 500000, 0, Fraction(0)
    for change in sorted(timeline["tempos"], key=lambda value: (value["tick"], value.get("track", -1), value.get("order", -1))):
        if change["tick"] > tick:
            break
        elapsed += Fraction((change["tick"] - cursor) * tempo, timeline["ticks_per_beat"])
        cursor, tempo = change["tick"], change["us_per_beat"]
    elapsed += Fraction((tick - cursor) * tempo, timeline["ticks_per_beat"])
    scaled = elapsed * rate / 1_000_000
    return (2 * scaled.numerator + scaled.denominator) // (2 * scaled.denominator)


def sine_sample() -> bytes:
    values = [int(6000 * math.sin(2 * math.pi * (frame % 100) / 100)) for frame in range(104)]
    return struct.pack("<104h", *values)


def resample_pcm16(raw: bytes, source_rate: int, target_rate: int) -> bytes:
    """Offline polyphase conversion; the mapping must explicitly choose its rate."""
    if source_rate == target_rate:
        return raw
    try:
        import numpy
        from scipy.signal import resample_poly
    except ModuleNotFoundError as error:
        raise CompileError("sf2_pcm16 resampling requires the recorded numpy/scipy tool dependency") from error
    divisor = math.gcd(source_rate, target_rate)
    samples = numpy.frombuffer(raw, dtype="<i2").astype(numpy.float64)
    converted = resample_poly(samples, target_rate // divisor, source_rate // divisor)
    return numpy.clip(numpy.rint(converted), -32768, 32767).astype("<i2").tobytes()


def aica_one_shot_tail(raw: bytes, rate: int) -> tuple[bytes, int, int]:
    """Make a non-looping recording safe for AICA's forward-loop playback."""
    frames, silence = len(raw) // 2, 256
    if frames + silence > 65535:
        raise CompileError(f"sample end {frames + silence - 1} exceeds AICA LEA; choose an explicit lower resample_hz")
    fade = min(frames, max(1, rate // 5))
    result = bytearray(raw + bytes(silence * 2))
    for frame in range(fade):
        offset = (frames - fade + frame) * 2
        value = struct.unpack_from("<h", result, offset)[0]
        struct.pack_into("<h", result, offset, round(value * (fade - frame - 1) / fade))
    return bytes(result), frames, frames + silence - 1


def aica_sf2_loop_bounds(start: int, end: int, source_rate: int, target_rate: int,
                         frames: int) -> tuple[int, int]:
    """Convert SoundFont's exclusive loop end to AICA's inclusive LEA."""
    loop_start = round(start * target_rate / source_rate)
    loop_end = round(end * target_rate / source_rate) - 1
    if not (0 <= loop_start < loop_end < frames and loop_end < 65535):
        raise CompileError(f"loop {loop_start}..{loop_end} is outside {frames} resampled frames; "
                           "choose an explicit lower resample_hz")
    return loop_start, loop_end


def trim_loop_pcm16(raw: bytes, loop_start: int, loop_end: int, rate: int,
                    loop_ms: int | None) -> tuple[bytes, int, int]:
    """Shorten an oversized looping recording at its least discontinuous seam."""
    if loop_ms is None or len(raw) // 2 <= rate * loop_ms // 1000:
        return raw, loop_start, loop_end
    target = min(loop_end, loop_start + rate * loop_ms // 1000)
    first = struct.unpack_from("<h", raw, loop_start * 2)[0]
    first_delta = struct.unpack_from("<h", raw, (loop_start + 1) * 2)[0] - first
    margin = rate * loop_ms // 4
    low, high = max(loop_start + 2, target - margin), min(loop_end, target + margin)
    if low > high:
        return raw, loop_start, loop_end
    end = min(range(low, high + 1), key=lambda frame: abs(struct.unpack_from("<h", raw, (frame - 1) * 2)[0] - first) + abs((struct.unpack_from("<h", raw, (frame - 1) * 2)[0] - struct.unpack_from("<h", raw, (frame - 2) * 2)[0]) - first_delta))
    # ponytail: choose a local PCM seam; use a loop crossfade only if an audition exposes it.
    return raw[:end * 2], loop_start, end - 1


def pcm16_to_pcm8(raw: bytes) -> bytes:
    """Lower signed little-endian PCM16 to the AICA's signed PCM8 format."""
    return bytes((struct.unpack_from("<h", raw, offset)[0] >> 8) & 0xff
                 for offset in range(0, len(raw), 2))


def pcm8_to_pcm16(raw: bytes) -> bytes:
    """Expand AICA signed PCM8 for offline encoder quality checks."""
    return struct.pack(f"<{len(raw)}h", *((value - 256 if value & 0x80 else value) << 8 for value in raw))


# AICA's fifth adaptation entry is 0x199, not a rounded decimal 408.
_ADPCM_QUANT = (230, 230, 230, 230, 307, 409, 512, 614)


def aica_adpcm_decode(raw: bytes, frames: int) -> bytes:
    """Decode AICA's low-nibble-first ADPCM for offline candidate evaluation."""
    sample, quant, values = 0, 0x7f, []
    for index in range(frames):
        nibble = (raw[index // 2] >> (4 * (index & 1))) & 15
        delta = quant * (1 + 2 * (nibble & 7)) // 8
        if nibble & 8: delta = -delta
        sample = max(-32768, min(32767, sample + delta))
        quant = max(0x7f, min(0x6000, quant * _ADPCM_QUANT[nibble & 7] >> 8))
        values.append(sample)
    return struct.pack(f"<{len(values)}h", *values)


def pcm16_to_adpcm_greedy(raw: bytes) -> bytes:
    """Reference encoder retained for the YA2BEAM quality regression check."""
    if len(raw) & 1: raise CompileError("ADPCM source must contain whole PCM16 frames")
    sample, quant, result = 0, 0x7f, bytearray()
    for index, (target,) in enumerate(struct.iter_unpack("<h", raw)):
        choices = []
        for nibble in range(16):
            delta = quant * (1 + 2 * (nibble & 7)) // 8
            if nibble & 8: delta = -delta
            choices.append((abs(target - max(-32768, min(32767, sample + delta))), nibble))
        _, nibble = min(choices)
        delta = quant * (1 + 2 * (nibble & 7)) // 8
        if nibble & 8: delta = -delta
        sample = max(-32768, min(32767, sample + delta))
        quant = max(0x7f, min(0x6000, quant * _ADPCM_QUANT[nibble & 7] >> 8))
        if index & 1: result[-1] |= nibble << 4
        else: result.append(nibble)
    return bytes(result)


def pcm16_to_adpcm(raw: bytes) -> bytes:
    """Encode PCM16 with the standard width-32 YA2BEAM AICA-ADPCM search."""
    try:
        return afx_adpcm.pcm16_to_adpcm(raw, beam_width=32)
    except ValueError as error:
        raise CompileError(str(error)) from error


def configured_sample_format(config: dict, key: str) -> int:
    formats = {"pcm16": PCM16, "pcm8": PCM8, "adpcm": ADPCM}
    value = config.get("sample_format", "pcm16")
    if value not in formats:
        raise CompileError(f"{key}: sample_format must be pcm16, pcm8 or adpcm")
    return formats[value]


def aica_rate(timecents: int | None, table: tuple) -> int:
    """Nearest KRS-off AICA rate, using MAME's documented AICA timing model."""
    if timecents is None: return 31
    target = 1000 * 2 ** (timecents / 1200)
    return min(range(1, 31), key=lambda rate: abs(math.log(table[2 * rate] / target)))


def aica_filter_level(cents: float) -> int:
    frequency = min(18000, max(20, 8.176 * 2 ** (max(-16000, min(16000, cents)) / 1200)))
    coefficient = 2 * math.sin(math.pi * frequency / AICA_NATIVE_SAMPLE_RATE)
    exponent = max(0, min(15, math.floor(math.log2(coefficient)) + 16))
    mantissa = round(coefficient * 2 ** (25 - exponent))
    return max(0, min(0x1ff7, exponent * 512 + mantissa - 512))


def aica_filter_rate(timecents: float, distance: int) -> int:
    if not distance: return 0
    target = 1000 * 2 ** (max(-12000, min(16000, timecents)) / 1200)
    return min(range(1, 32), key=lambda rate: abs(math.log(DR_TIME_MS[2 * rate] * abs(distance) / 1024 / target)))


def aica_envelope(controls: dict, mode: str, config: dict | None = None) -> tuple[int, int]:
    config = config or {}
    if mode == "aica_explicit_rates_v1":
        ar, rr = config.get("attack_rate"), config.get("release_rate")
        if not all(isinstance(rate, int) and 1 <= rate <= 30 for rate in (ar, rr)):
            raise CompileError("aica_explicit_rates_v1 requires attack_rate and release_rate in 1..30")
        return ar, rr | 0x3c00  # KRS=15: authored rates do not vary with note pitch.
    if mode == "aica_mame_v1":
        ar = aica_rate(controls["attack_timecents"], AR_TIME_MS)
        rr = aica_rate(controls["release_timecents"], DR_TIME_MS)
        return ar, rr | 0x3c00  # KRS=15: source time does not change with note pitch.
    if mode != "aica_sf2_adsr_v1":
        raise CompileError("sf2_pcm16 envelope must be aica_mame_v1, aica_explicit_rates_v1 or aica_sf2_adsr_v1")
    fallback_attack = config.get("attack_rate", 31)
    release_override = config.get("release_rate") is not None
    fallback_release = config["release_rate"] if release_override else 31
    d1r_override = config.get("decay_rate")
    dl_override = config.get("decay_level")
    d2r, krs = config.get("sustain_decay_rate", 0), config.get("key_rate_scale", 15)
    if not all(isinstance(rate, int) and 0 <= rate <= 31
               for rate in (fallback_attack, fallback_release, d2r, krs)):
        raise CompileError("aica_sf2_adsr_v1 rates must be integers in 0..31")
    if any(rate is not None and (not isinstance(rate, int) or not 0 <= rate <= 31)
           for rate in (d1r_override, dl_override)):
        raise CompileError("aica_sf2_adsr_v1 decay_rate and decay_level must be integers in 0..31")
    ar = aica_rate(controls["attack_timecents"], AR_TIME_MS) \
        if controls["attack_timecents"] is not None else fallback_attack
    if "attack_rate_override" in config:
        ar = config["attack_rate_override"]
        if not isinstance(ar, int) or not 1 <= ar <= 30:
            raise CompileError("attack_rate_override must be an integer in 1..30")
    d1r = d1r_override if d1r_override is not None else aica_rate(controls["decay_timecents"], DR_TIME_MS)
    rr = fallback_release if release_override else aica_rate(controls["release_timecents"], DR_TIME_MS)
    sustain = controls["sustain_centibels"]
    dl = dl_override if dl_override is not None else \
         (0 if sustain is None else max(0, min(31, round(sustain * 31 / 1440))) )
    if dl_override is None and sustain is not None and config.get("gain_model") == "fluidsynth_2":
        # DL advances eight TL steps: one half-amplitude (3.0103 dB) each.
        dl = max(0, min(31, round(sustain / (100 * math.log10(2)))))
    return ar | d1r << 6 | d2r << 11, rr | dl << 5 | krs << 10


def aica_triangle_lfo(config: dict) -> int:
    """Pack the documented AICA triangle PLFO/ALFO register from explicit mapping."""
    lfo = config.get("lfo")
    if lfo is None:
        return 0
    if not isinstance(lfo, dict):
        raise CompileError("lfo must be an object with rate, pitch_depth and amplitude_depth")
    rate, pitch_depth, amplitude_depth = (lfo.get(name) for name in
                                          ("rate", "pitch_depth", "amplitude_depth"))
    if not all(isinstance(value, int) for value in (rate, pitch_depth, amplitude_depth)) or \
       not 0 <= rate <= 31 or not 0 <= pitch_depth <= 7 or not 0 <= amplitude_depth <= 7:
        raise CompileError("lfo rate must be 0..31; pitch_depth and amplitude_depth must be 0..7")
    # Slot 0x1c: LFOF[14:10], PLFOWS[9:8], PLFOS[7:5], ALFOWS[4:3], ALFOS[2:0].
    return rate << 10 | 2 << 8 | pitch_depth << 5 | 2 << 3 | amplitude_depth


def midi_modulation_policy(config: dict) -> str:
    policy = config.get("midi_modulation", "ignore")
    if policy not in ("ignore", "pitch", "amplitude", "both"):
        raise CompileError("midi_modulation must be ignore, pitch, amplitude or both")
    if policy != "ignore" and config.get("lfo") is None:
        raise CompileError("midi_modulation needs an lfo object with maximum depths")
    return policy


def record_lfo(note: dict, record: dict, controller: dict | None = None) -> int:
    if record.get("midi_modulation") == "ignore": return record["lfo"]
    value = (note if controller is None else controller).get("channel_modulation", 0)
    if not isinstance(value, int) or not 0 <= value <= 127:
        raise CompileError("MIDI modulation must be in 0..127")
    lfo = dict(record["lfo_config"])
    policy = record["midi_modulation"]
    if policy in ("pitch", "both"):
        lfo["pitch_depth"] = round(lfo["pitch_depth"] * value / 127)
    if policy in ("amplitude", "both"):
        lfo["amplitude_depth"] = round(lfo["amplitude_depth"] * value / 127)
    return aica_triangle_lfo({"lfo": lfo})


def aica_total_level(attenuation_centibels: float, lpf: int = 0x24) -> int:
    """Pack AICA slot 0x28: TL is 0.4 dB per step; LPF is its low byte."""
    if not isinstance(lpf, int) or not 0 <= lpf <= 255:
        raise CompileError("mix must be an AICA LPF byte in 0..255")
    return min(255, max(0, round(attenuation_centibels / 40))) << 8 | lpf


def sample_attenuation_centibels(config: dict, sample) -> float:
    """Return the optional offline gain correction for one named SF2 sample."""
    overrides = config.get("sample_attenuation_centibels", {})
    if not isinstance(overrides, dict):
        raise CompileError("sample_attenuation_centibels must be an object")
    value = overrides.get(sample.name, 0)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CompileError(f"{sample.name}: sample attenuation must be a number of centibels")
    return float(value)


def sample_tuning_cents(config: dict, sample) -> int:
    """Return an offline pitch correction for one named SoundFont sample region."""
    overrides = config.get("sample_tuning_cents", {})
    if not isinstance(overrides, dict):
        raise CompileError("sample_tuning_cents must be an object")
    value = overrides.get(sample.name, 0)
    if isinstance(value, bool) or not isinstance(value, int) or not -2400 <= value <= 2400:
        raise CompileError(f"{sample.name}: sample tuning must be integer cents in -2400..2400")
    return value


def midi_filter_policy(config: dict) -> str:
    policy = config.get("midi_filter", "ignore")
    if policy not in ("apply", "ignore"):
        raise CompileError("midi_filter must be apply or ignore")
    return policy


def midi_filter_value(value: object, name: str, maximum: int) -> int:
    if not isinstance(value, int) or not 0 <= value <= 127:
        raise CompileError(f"MIDI {name} must be in 0..127")
    return round(value * maximum / 127)


def record_lpf(note: dict, record: dict, controller: dict | None = None) -> int:
    if record.get("midi_filter") != "apply": return record["lpf"]
    return midi_filter_value((note if controller is None else controller).get("channel_brightness", 64),
                             "brightness", 15)


def sf2_velocity_attenuation_centibels(velocity: int) -> float:
    """The standard SF2 concave velocity response, approximated in centibels."""
    if not 1 <= velocity <= 127:
        raise CompileError(f"MIDI note velocity {velocity} is outside 1..127")
    return max(0.0, -300.0 * math.log10(velocity / 127.0))


def sf2_pan(controls: dict | None, channel_pan: int) -> tuple[int, float]:
    pan = max(-500, min(500, ((controls or {}).get("pan_centibels") or 0) + (channel_pan - 64) * 500 / 63))
    angle = (pan + 500) * math.pi / 2000
    left, right = math.cos(angle), math.sin(angle)
    loud, soft = max(left, right), min(left, right)
    steps = min(15, round(-20 * math.log10(max(soft / loud, 1e-9)) / (10 * math.log10(2))))
    return (steps | (0x10 if pan <= 0 else 0)), -2000 * math.log10(loud * math.sqrt(2))


def aica_direct(config: dict, controls: dict | None = None, channel_pan: int = 64) -> int:
    """Pack slot 0x24: direct pan low five bits and filter-Q bits 8..12."""
    value = int(config.get("direct", 0x0f10))
    if not 0 <= value <= 0xffff:
        raise CompileError("direct must be an AICA 16-bit pan/filter-Q word")
    q = int(config.get("filter_q", (value >> 8) & 0x1f))
    if not 0 <= q <= 31: raise CompileError("filter_q must be in 0..31")
    source_pan = config.get("source_pan", "apply")
    if source_pan not in ("apply", "ignore"):
        raise CompileError("source_pan must be apply or ignore")
    if not isinstance(channel_pan, int) or not 0 <= channel_pan <= 127:
        raise CompileError("MIDI channel pan must be in 0..127")
    if source_pan == "apply":
        if config.get("gain_model") == "fluidsynth_2" and config.get("pan_model", "sf2") == "sf2":
            return q << 8 | sf2_pan(controls, channel_pan)[0]
        source_steps = 0 if controls is None or controls.get("pan_centibels") is None else \
            round(controls["pan_centibels"] / 300)
        controller_steps = round((channel_pan - 64) * 15 / 63)
        if source_steps or controller_steps:
            steps = min(15, max(-15, source_steps + controller_steps))
            # SF2 positive means right. AICA 0x01..0x0f attenuate left; 0x11..0x1f right.
            value = (value & ~0x1f) | (steps if steps >= 0 else (0x10 | -steps))
    return q << 8 | value & 0x1f


def aica_dsp_send(config: dict, controls: dict | None = None) -> int:
    """Pack slot 0x20's IMXL/ISEL byte for the scene-global DSP."""
    source_reverb = config.get("source_reverb", "ignore")
    if source_reverb not in ("apply", "ignore"):
        raise CompileError("source_reverb must be apply or ignore")
    send = config.get("dsp_send")
    if send is not None:
        if source_reverb == "apply":
            raise CompileError("dsp_send conflicts with source_reverb apply")
        if not isinstance(send, int) or not 0 <= send <= 0xff:
            raise CompileError("dsp_send must be an AICA reverb/chorus byte in 0..255")
        return send
    if source_reverb == "apply":
        if config.get("dsp_bus", 0) != 0:
            raise CompileError("source_reverb apply requires dsp_bus 0")
        send = 0 if controls is None else controls.get("reverb_send_per_mille") or 0
        if not isinstance(send, int) or not 0 <= send <= 1000:
            raise CompileError("SF2 reverb send must be in 0..1000")
        # The room consumes MIXS[0]: IMXL is the high nibble, ISEL is zero.
        return round(send * 15 / 1000) << 4
    return 0


def midi_reverb_policy(config: dict) -> str:
    policy = config.get("midi_reverb", "ignore")
    if policy not in ("apply", "ignore"):
        raise CompileError("midi_reverb must be apply or ignore")
    if policy == "apply":
        if config.get("dsp_bus", 0) != 0:
            raise CompileError("midi_reverb apply requires dsp_bus 0")
        if config.get("source_reverb", "ignore") == "apply" or "dsp_send" in config:
            raise CompileError("midi_reverb apply conflicts with source_reverb apply or dsp_send")
    return policy


def midi_chorus_policy(config: dict) -> str:
    policy = config.get("midi_chorus", "ignore")
    if policy not in ("apply", "ignore"):
        raise CompileError("midi_chorus must be apply or ignore")
    if policy == "apply":
        if config.get("dsp_bus", 0) != 0:
            raise CompileError("midi_chorus apply requires dsp_bus 0")
        if config.get("midi_reverb", "ignore") == "apply" or config.get("source_reverb", "ignore") == "apply" or "dsp_send" in config:
            raise CompileError("midi_chorus apply conflicts with midi_reverb, source_reverb, or dsp_send")
    return policy


def record_dsp_send(note: dict, record: dict, controller: dict | None = None) -> int:
    effect = "reverb" if record.get("midi_reverb") == "apply" else "chorus" if record.get("midi_chorus") == "apply" else None
    if effect is None: return record["dsp_send"]
    value = (note if controller is None else controller).get(f"channel_{effect}", 0)
    if not isinstance(value, int) or not 0 <= value <= 127:
        raise CompileError("MIDI channel effect send must be in 0..127")
    # CC91/CC93 control the MIXS[0] send level, not the bus selector.
    return round(value * 15 / 127) << 4


def midi_controller_attenuation_centibels(note: dict) -> float:
    volume, expression = note.get("channel_volume", 127), note.get("channel_expression", 127)
    if not all(isinstance(value, int) and 0 <= value <= 127 for value in (volume, expression)):
        raise CompileError("MIDI channel volume and expression must be in 0..127")
    if not volume or not expression: return 10200
    return -2000 * math.log10(volume * expression / (127 * 127))


def record_total_level(note: dict, record: dict, controller: dict | None = None,
                       attenuation_delta_centibels: float = 0) -> int:
    controls = record.get("source_controls")
    source = record.get("base_attenuation", 0 if controls is None else controls["attenuation_centibels"] or 0)
    velocity = record.get("velocity_attenuation", 0 if controls is None else
                          sf2_velocity_attenuation_centibels(note["velocity"]))
    attenuation = source + velocity + attenuation_delta_centibels + record.get("controller_gain_scale", 1) * midi_controller_attenuation_centibels(
                            note if controller is None else controller)
    config = record.get("direct_config", {})
    if config.get("gain_model") == "fluidsynth_2" and config.get("pan_model", "sf2") == "sf2" and config.get("source_pan", "apply") == "apply":
        attenuation += sf2_pan(controls, (note if controller is None else controller).get("channel_pan", 64))[1]
    return aica_total_level(attenuation * record.get("tl_gain_scale", 1), record_lpf(note, record, controller))


def record_direct(record: dict, controller: dict) -> int:
    value = aica_direct(record["direct_config"], record.get("source_controls"), controller["channel_pan"])
    if record.get("midi_filter") == "apply":
        value = (midi_filter_value(controller.get("channel_resonance", 64), "resonance", 31) << 8 |
                 value & 0x1f)
    return value


def baked_level_curve(config: dict) -> tuple[tuple[int, float], ...]:
    """Explicit per-instrument approximation points; ARM7 only receives PATCH writes."""
    curve = config.get("baked_level_curve", [])
    if not isinstance(curve, list):
        raise CompileError("baked_level_curve must be an array")
    result, previous = [], 0
    for point in curve:
        if not isinstance(point, dict):
            raise CompileError("baked_level_curve points must be objects")
        at_ms, delta = point.get("at_ms"), point.get("attenuation_delta_centibels")
        if not isinstance(at_ms, int) or not 1 <= at_ms <= 600000:
            raise CompileError("baked_level_curve at_ms must be 1..600000")
        if not isinstance(delta, (int, float)) or not -9600 <= delta <= 10200:
            raise CompileError("baked_level_curve attenuation_delta_centibels must be -9600..10200")
        if at_ms <= previous:
            raise CompileError("baked_level_curve points must have strictly increasing at_ms")
        previous = at_ms
        result.append((at_ms, float(delta)))
    return tuple(result)


def controller_state_at(timeline: dict, note: dict, tick_rate: int, when: int) -> dict:
    """Resolve the MIDI controller state seen by a compiler-authored update point."""
    state = {name: note[name] for name in ("channel_volume", "channel_expression", "channel_pan",
                                            "channel_reverb", "channel_modulation", "pitch_bend",
                                            "pitch_bend_range_cents", "channel_resonance",
                                            "channel_brightness")}
    state["channel_chorus"] = note.get("channel_chorus", 0)
    start = control_start(timeline, note, tick_rate)
    for controller in timeline.get("automation", []):
        if controller["channel"] != note["channel"]: continue
        control_tick = tick_to_control_ticks(timeline, controller["tick"], tick_rate)
        if start < control_tick <= when:
            state.update({name: controller[name] for name in state if name in controller})
    return state


def timed_baked_level_patches(timeline: dict, records: list[dict], tick_rate: int) -> list[tuple]:
    """Spend explicit offline-authored writes where an AICA envelope is insufficient."""
    patches = []
    for note, record in zip(timeline["notes"], records):
        for at_ms, delta in record.get("baked_level_curve", ()):
            when = control_start(timeline, note, tick_rate) + round(at_ms * tick_rate / 1000)
            if when >= control_end(timeline, note, tick_rate): continue
            patches.append((when, 3, note["local_channel"], note["id"],
                            {"curve": (note, record, delta)}))
    return patches


def spread_baked_level_patches(timeline: dict, events: list[tuple], mapping: dict,
                               tick_rate: int) -> tuple[list[tuple], dict | None]:
    """Bound authored curve-update bursts without moving source MIDI controls."""
    limit, delay = (mapping.get("baked_level_curve_patch_limit"),
                    mapping.get("baked_level_curve_max_delay_ticks"))
    policy = limit is not None or delay is not None
    if policy:
        if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 64:
            raise CompileError("baked_level_curve_patch_limit must be an integer in 1..64")
        if isinstance(delay, bool) or not isinstance(delay, int) or not 0 <= delay <= 1000:
            raise CompileError("baked_level_curve_max_delay_ticks must be an integer in 0..1000")
    end = {note["id"]: control_end(timeline, note, tick_rate) for note in timeline["notes"]}
    occupied, result, maximum_delay = {}, [], 0
    for event in sorted((event for event in events if event[1] == 3), key=lambda event: event[:4]):
        when, _, channel, note_id, payload = event
        scheduled = when if not policy else next((candidate for candidate in
                                                  range(when, min(end[note_id], when + delay + 1))
                                                  if occupied.get(candidate, 0) < limit), None)
        if scheduled is None:
            raise CompileError(f"baked level curve budget exhausted for note {note_id}; "
                               "reduce points, raise the patch limit, or allow more delay")
        if policy: occupied[scheduled] = occupied.get(scheduled, 0) + 1
        maximum_delay = max(maximum_delay, scheduled - when)
        if "curve" in payload:
            note, record, delta = payload["curve"]
            state = controller_state_at(timeline, note, tick_rate, scheduled)
            payload = {AFX_FIELD_TOTAL_LEVEL: record_total_level(note, record, state, delta)}
        result.append((scheduled, 3, channel, note_id, payload))
    result.extend(event for event in events if event[1] != 3)
    return result, {"limit": limit, "max_delay_ticks": maximum_delay} if policy else None


def timed_controller_patches(timeline: dict, records: list[dict], tick_rate: int) -> list[tuple]:
    """Coalesce one channel's same-tick MIDI controls into one active-voice PATCH."""
    patches = {}
    for controller in timeline.get("automation", []):
        is_pitch = controller.get("kind") == "pitch"
        is_filter_cutoff = controller.get("kind") == "filter_cutoff"
        reset = controller.get("control") == 121
        if not is_pitch and not is_filter_cutoff and not reset and controller.get("control") not in (1, 7, 10, 11, 71, 74, 91, 93): continue
        when = tick_to_control_ticks(timeline, controller["tick"], tick_rate)
        source = (controller["tick"], controller["track"], controller["order"])
        for note, record in zip(timeline["notes"], records):
            start_source = (note["start_tick"], note["track"], note["order"])
            end_source = (note["end_tick"], note.get("end_track", 16), note.get("end_order", 0))
            if note["channel"] != controller["channel"] or not start_source < source < end_source:
                continue
            fields = {}
            if is_pitch or reset:
                fields[AFX_FIELD_PITCH] = record_pitch(note, record, controller)
            if controller.get("control") in (7, 11) or reset:
                fields[AFX_FIELD_TOTAL_LEVEL] = record_total_level(note, record, controller)
            if controller.get("control") == 10:
                fields[AFX_FIELD_DIRECT] = record_direct(record, controller)
                if record.get("direct_config", {}).get("gain_model") == "fluidsynth_2":
                    fields[AFX_FIELD_TOTAL_LEVEL] = record_total_level(note, record, controller)
            if controller.get("control") == 91 and record.get("midi_reverb") == "apply":
                fields[AFX_FIELD_DSP_SEND] = record_dsp_send(note, record, controller)
            if controller.get("control") == 93 and record.get("midi_chorus") == "apply":
                fields[AFX_FIELD_DSP_SEND] = record_dsp_send(note, record, controller)
            if controller.get("control") in (1, 121) and record.get("midi_modulation") != "ignore":
                fields[AFX_FIELD_LFO] = record_lfo(note, record, controller)
            if controller.get("control") == 71 and record.get("midi_filter") == "apply":
                fields[AFX_FIELD_DIRECT] = record_direct(record, controller)
            if (controller.get("control") == 74 or is_filter_cutoff) and record.get("midi_filter") == "apply":
                fields[AFX_FIELD_TOTAL_LEVEL] = record_total_level(note, record, controller)
            key = (max(when, control_start(timeline, note, tick_rate)),
                   note["local_channel"], note["id"])
            patches.setdefault(key, {}).update(fields)
    return [(when, 2, channel, note_id, fields)
            for (when, channel, note_id), fields in patches.items()]


def resolve_instruments(timeline: dict, mapping: dict) -> tuple[dict[str, dict], list[dict]]:
    instruments = mapping.get("instruments")
    if not isinstance(instruments, dict):
        raise CompileError("mapping must contain an instruments object")
    needed = sorted({mapping_key(note) for note in timeline["notes"]})
    missing = [key for key in needed if key not in instruments]
    if missing:
        available = ", ".join(sorted(instruments)) or "none"
        raise CompileError(f"missing instrument mappings: {', '.join(missing)}; available: {available}")
    warnings = []
    resolved = {}
    for key in needed:
        value = instruments[key]
        if not isinstance(value, dict) or value.get("kind") != "sine":
            raise CompileError(f"{key}: only kind=sine is implemented; available mapping kinds are explicit")
        if value.get("velocity") != "fixed" or value.get("release") != "keyoff":
            raise CompileError(f"{key}: declare velocity=fixed and release=keyoff for the sine bring-up instrument")
        resolved[key] = {
            "kind": "sine", "root_key": int(value.get("root_key", 69)),
            "total_level": aica_total_level(float(value.get("attenuation_centibels", 0)), int(value.get("mix", 0x24)) & 0xff),
            "dsp_send": aica_dsp_send(value), "direct": aica_direct(value),
            "midi_reverb": midi_reverb_policy(value), "midi_chorus": midi_chorus_policy(value),
            "lfo": aica_triangle_lfo(value), "midi_modulation": midi_modulation_policy(value),
        }
        warnings.append({"kind": "explicit_fixed_velocity", "instrument": key,
                         "detail": "sine bring-up mapping uses its declared fixed mix"})
    return resolved, warnings


def sf2_groups(timeline: dict, mapping: dict) -> dict:
    """Validate SF2 mappings and group notes by their one source preset."""
    instruments = mapping.get("instruments")
    if not isinstance(instruments, dict):
        raise CompileError("mapping must contain an instruments object")
    needed = sorted({mapping_key(note) for note in timeline["notes"]})
    missing = [key for key in needed if key not in instruments]
    if missing:
        available = ", ".join(sorted(instruments)) or "none"
        raise CompileError(f"missing instrument mappings: {', '.join(missing)}; available: {available}")
    if any(instrument_config(mapping, note).get("kind") != "sf2_pcm16" for note in timeline["notes"]):
        raise CompileError("sf2_pcm16 compile cannot mix instrument kinds")
    groups = {}
    embedded_sources = {}
    for index, note in enumerate(timeline["notes"]):
        config = instrument_config(mapping, note)
        if config.get("velocity") != "sf2" or config.get("release") != "keyoff":
            raise CompileError(f"{mapping_key(note)}: declare velocity=sf2 and release=keyoff")
        envelope_mode = config.get("envelope", "fixed")
        if envelope_mode not in ("fixed", "aica_mame_v1", "aica_explicit_rates_v1", "aica_sf2_adsr_v1"):
            raise CompileError(f"{mapping_key(note)}: unsupported sf2_pcm16 envelope mode {envelope_mode}")
        try:
            preset = afx_sf2.parse_pair(str(config["preset"]))
            channel = config["channel"]
        except (KeyError, ValueError) as error:
            raise CompileError(f"{mapping_key(note)}: sf2_pcm16 requires a bank, preset and channel") from error
        if channel not in ("left", "right", "stereo"):
            raise CompileError(f"{mapping_key(note)}: sf2_pcm16 channel must be explicit left, right or stereo")
        configured_sample_format(config, mapping_key(note))
        if "soundfont_bytes" in config:
            data = config["soundfont_bytes"]
            if not isinstance(data, bytes): raise CompileError(f"{mapping_key(note)}: embedded bank is not bytes")
            source = embedded_sources.get(id(data))
            if source is None:
                source = ("embedded", hashlib.sha256(data).hexdigest())
                embedded_sources[id(data)] = source
            label = str(config.get("soundfont_name", "<embedded SoundFont>"))
        elif "soundfont" in config:
            source = ("path", str(Path(config["soundfont"])))
            data, label = None, source[1]
        else:
            raise CompileError(f"{mapping_key(note)}: sf2_pcm16 requires soundfont or embedded bank_input")
        group = groups.setdefault((source, preset, channel), {"data": data, "label": label, "entries": []})
        group["entries"].append((index, note, config))
    return groups


def expand_sf2_layers(timeline: dict, mapping: dict) -> None:
    """Lower every selected SF2 zone into an independently colourable voice."""
    layered = []
    for ((source, preset_id, channel), group) in sf2_groups(timeline, mapping).items():
        if group["data"] is not None:
            input_file = BytesIO(group["data"])
        else:
            try:
                input_file = Path(source[1]).open("rb")
            except OSError as error:
                raise CompileError(f"SoundFont {group['label']}: {error}") from error
        with input_file:
            font = afx_sf2.Sf2File(input_file)
            preset = next((item for item in font.presets
                           if item.bank == preset_id[0] and item.preset == preset_id[1]), None)
            if not preset:
                available = ", ".join(f"{item.bank}:{item.preset}" for item in font.presets)
                raise CompileError(f"SoundFont preset {preset_id} missing; available: {available or 'none'}")
            for _, note, _ in group["entries"]:
                uses = afx_sf2.select_note_uses(note, preset, channel)
                if not uses:
                    raise CompileError(f"note {note['id']}: selected no {channel} SF2 zones")
                for layer in range(len(uses)):
                    layered.append({**note, "source_note_id": note["id"], "_sf2_layer": layer})
    layered.sort(key=lambda note: (note["start_tick"], note["track"], note["order"],
                                   note["source_note_id"], note["_sf2_layer"]))
    for index, note in enumerate(layered): note["id"] = index
    timeline["notes"] = layered


def _sf2_records(timeline: dict, mapping: dict) -> list[dict]:
    """Materialize each independently coloured selected SF2 zone."""
    records = [None] * len(timeline["notes"])
    groups = sf2_groups(timeline, mapping)
    for ((source, preset_id, channel), group) in groups.items():
        if group["data"] is not None:
            input_file = BytesIO(group["data"])
        else:
            try:
                input_file = Path(source[1]).open("rb")
            except OSError as error:
                raise CompileError(f"SoundFont {group['label']}: {error}") from error
        with input_file:
            font = afx_sf2.Sf2File(input_file)
            preset = next((item for item in font.presets
                           if item.bank == preset_id[0] and item.preset == preset_id[1]), None)
            if not preset:
                available = ", ".join(f"{item.bank}:{item.preset}" for item in font.presets)
                raise CompileError(f"SoundFont preset {preset_id} missing; available: {available or 'none'}")
            converted = {}
            for index, note, config in group["entries"]:
                gain_model = config.get("gain_model", "legacy")
                if gain_model not in ("legacy", "fluidsynth_2"):
                    raise CompileError("gain_model must be legacy or fluidsynth_2")
                if config.get("pan_model", "sf2") not in ("sf2", "legacy"):
                    raise CompileError("pan_model must be sf2 or legacy")
                filter_level = config.get("filter_level", 0x1fff)
                if not isinstance(filter_level, int) or not 0 <= filter_level <= 0x1fff:
                    raise CompileError("filter_level must be an AICA cutoff level in 0..8191")
                if config.get("filter_model") not in (None, "sf2_static", "sf2_envelope"):
                    raise CompileError("filter_model must be sf2_static or sf2_envelope")
                filter_offset = config.get("filter_offset_cents", 0)
                if not isinstance(filter_offset, (int, float)) or not math.isfinite(filter_offset) or abs(filter_offset) > 12000:
                    raise CompileError("filter_offset_cents must be finite in -12000..12000")
                if type(config.get("fit_sample_frames", False)) is not bool:
                    raise CompileError("fit_sample_frames must be boolean")
                loop_ms = config.get("loop_ms")
                if loop_ms is not None and (not isinstance(loop_ms, int) or not 20 <= loop_ms <= 2000):
                    raise CompileError("loop_ms must be an integer in 20..2000")
                envelope_mode = config.get("envelope", "fixed")
                uses = afx_sf2.select_note_uses(note, preset, channel)
                layer = note.get("_sf2_layer")
                if not isinstance(layer, int) or not 0 <= layer < len(uses):
                    raise CompileError(f"note {note['id']}: invalid selected SF2 layer")
                use, sample = uses[layer], uses[layer]["sample"]
                filter_q = int(config.get("mix", 0x24)) & 0xff
                filter_env_ad = filter_env_dr = 0
                filter_levels = [filter_level] * 5
                if config.get("filter_model") in ("sf2_static", "sf2_envelope"):
                    controls = use["source_controls"]
                    cutoff = controls["filter_cutoff_cents"] if controls["filter_cutoff_cents"] is not None else 13500
                    cutoff += use["velocity_filter_amount"] * (127 - note["velocity"]) / 128
                    cutoff += filter_offset
                    filter_level = aica_filter_level(cutoff)
                    filter_levels = [filter_level] * 5
                    filter_q = max(0, min(15, round((controls["filter_q_centibels"] or 0) / 7.5 + 4)))
                    if config.get("filter_model") == "sf2_envelope":
                        amount = controls["mod_env_filter_cents"] or 0
                        peak = aica_filter_level(cutoff + amount)
                        sustain = aica_filter_level(cutoff + amount * (1 - min(1000, max(0, controls["mod_env_sustain_per_mille"] or 0)) / 1000))
                        filter_levels = [filter_level, peak, sustain, sustain, filter_level]
                        decay = (controls["mod_env_decay_timecents"] if controls["mod_env_decay_timecents"] is not None else -12000)
                        decay += (controls["mod_env_key_decay_timecents"] or 0) * (60 - note["key"])
                        attack = controls["mod_env_attack_timecents"] if controls["mod_env_attack_timecents"] is not None else -12000
                        release = controls["mod_env_release_timecents"] if controls["mod_env_release_timecents"] is not None else -12000
                        # ponytail: native linear cutoff ramps approximate SF2's
                        # modulation envelope; add hold only for banks needing it.
                        filter_env_ad = aica_filter_rate(attack, peak - filter_level) << 8 | aica_filter_rate(decay, peak - sustain)
                        filter_env_dr = aica_filter_rate(release, peak - filter_level)
                rate = config.get("resample_hz")
                if config.get("fit_sample_frames") is True and isinstance(rate, int):
                    # Preserve pitch while using the highest requested rate that
                    # fits this sample's 16-bit loop/end addresses (plus tail guard).
                    rate = min(rate, sample.sample_rate,
                               ((65500 if use["loop"] else 65279) * sample.sample_rate) // (sample.end - sample.start))
                if not isinstance(rate, int) or not 4000 <= rate <= sample.sample_rate:
                    raise CompileError(f"{mapping_key(note)}: declare resample_hz in 4000..{sample.sample_rate}")
                sample_format = configured_sample_format(config, mapping_key(note))
                identity = (sample.start, sample.end, sample.sample_type, rate, bool(use["loop"]), loop_ms, sample_format)
                if identity not in converted:
                    raw = resample_pcm16(bytes(sample.raw_sample_data), sample.sample_rate, rate)
                    frames = len(raw) // 2
                    if use["loop"]:
                        loop_start, loop_end = aica_sf2_loop_bounds(sample.start_loop, sample.end_loop,
                                                                      sample.sample_rate, rate, frames)
                        raw, loop_start, loop_end = trim_loop_pcm16(raw, loop_start, loop_end, rate, loop_ms)
                        frames = len(raw) // 2
                    else:
                        raw, loop_start, loop_end = aica_one_shot_tail(raw, rate)
                        frames = len(raw) // 2
                    if sample_format == PCM8: raw = pcm16_to_pcm8(raw)
                    if sample_format == ADPCM: raw = pcm16_to_adpcm(raw)
                    converted[identity] = (raw, frames, loop_start, loop_end)
                raw, frames, loop_start, loop_end = converted[identity]
                env_ad, env_dr = (0x001f, 0x001f) if envelope_mode == "fixed" else \
                                 aica_envelope(use["source_controls"], envelope_mode, config)
                record = {"raw": raw, "format": sample_format, "frames": frames,
                                  "loop_start": loop_start, "loop_end": loop_end,
                                  "loop": True,
                                  "source_loop": bool(use["loop"]),
                                  "root_key": use["root_key"], "tuning_cents": use["tuning_cents"] + sample_tuning_cents(config, sample),
                                  "rate_tuning_cents": aica_rate_tuning_cents(rate),
                                  "source_controls": use["source_controls"],
                                  "filter_level": filter_level,
                                  "filter_model": config.get("filter_model"),
                                  "filter_levels": tuple(filter_levels), "filter_env_ad": filter_env_ad, "filter_env_dr": filter_env_dr,
                                  "base_attenuation": (float(config.get("attenuation_centibels", 0)) +
                                                       sample_attenuation_centibels(config, sample) +
                                                       (use["source_controls"]["attenuation_centibels"] or 0) *
                                                       (4 if gain_model == "fluidsynth_2" else 1)),
                                  "source_reverb_applied": config.get("source_reverb", "ignore") == "apply",
                                  "release_rate_overridden": config.get("release_rate") is not None,
                                  "envelope_mode": envelope_mode, "env_ad": env_ad, "env_dr": env_dr,
                                  "lfo": aica_triangle_lfo(config), "lfo_config": config.get("lfo"),
                                  "midi_modulation": midi_modulation_policy(config),
                                  "total_level": aica_total_level(0, int(config.get("mix", 0x24)) & 0xff),
                                  "lpf": filter_q,
                                  "dsp_send": aica_dsp_send(config, use["source_controls"]),
                                  "dsp_config": config, "midi_reverb": midi_reverb_policy(config), "midi_chorus": midi_chorus_policy(config),
                                  "midi_filter": midi_filter_policy(config),
                                  "direct_config": config,
                                  "direct": aica_direct(config, use["source_controls"], note.get("channel_pan", 64)),
                                  "baked_level_curve": baked_level_curve(config),
                                  "name": sample.name}
                record["dsp_send"] = record_dsp_send(note, record)
                if gain_model == "fluidsynth_2":
                    # FluidSynth 2.6: default concave velocity/CC modulators give
                    # -40 log10(value/127) dB. The historical authoring gain unit
                    # here is 0.01 dB; source initialAttenuation uses EMU's 0.04 dB.
                    record["velocity_attenuation"] = (-4000 * math.log10(note["velocity"] / 127) *
                                                      use["velocity_attenuation_amount"] / 960)
                    record["controller_gain_scale"] = 2
                    # Native TL halves amplitude every 16 steps. Preserve the
                    # legacy 0.4 dB authoring conversion for existing mappings.
                    record["tl_gain_scale"] = 40 / (2000 * math.log10(2) / 16)
                record["lfo"] = record_lfo(note, record)
                record["direct"] = record_direct(record, note)
                record["total_level"] = record_total_level(note, record)
                records[index] = record
    return records


def optimize_setup_dictionary(records: list[dict], fixed_key, variant_fields: tuple[str, ...],
                              dynamic_key=None) -> tuple[list[dict], dict]:
    """Share fixed sound state and override rare per-note setup variants."""
    groups = {}
    original = set()
    for record in records:
        fixed = fixed_key(record)
        variant = tuple(record[field] for field in variant_fields)
        original.add((fixed, variant, dynamic_key(record) if dynamic_key else None))
        groups.setdefault(fixed, {}).setdefault(variant, []).append(record)
    setups, extra_bytes = [], 0
    for variants in groups.values():
        retained = []
        # ponytail: greedy O(v²) search within a compatible sample/envelope group;
        # use a global dictionary search only if measured remaining savings justify it.
        for variant, uses in sorted(variants.items(), key=lambda item: (-len(item[1]), item[0])):
            best = None
            for reference, index in retained:
                differences = sum(a != b for a, b in zip(variant, reference))
                cost = len(uses) * (4 + 2 * differences)
                if best is None or (cost, index) < best:
                    best = cost, index
            # Also shrink resident RAM, not just the file's 12-byte relocation table.
            if best is not None and best[0] < SETUP_BYTES:
                cost, index = best
                extra_bytes += cost
            else:
                index = len(setups)
                setups.append(uses[0])
                retained.append((variant, index))
            for record in uses:
                record["setup"] = index
    return setups, {"original_setups": len(original), "optimized_setups": len(setups),
                    "extra_note_bytes": extra_bytes,
                    "level_variants_removed": len(original) - sum(len(v) for v in groups.values()),
                    "encoding_bytes_saved": (len(original) - len(setups)) * (SETUP_BYTES + 12) - extra_bytes}


def optimize_record_setups(records: list[dict]) -> tuple[list[dict], dict]:
    """MIDI records use the common setup dictionary with their fixed SCSP state."""
    return optimize_setup_dictionary(
        records,
        lambda record: (record["sample"], record["loop"], record["loop_start"], record["loop_end"],
                        record["env_ad"], record["env_dr"],
                        tuple(record.get("filter_levels", [record.get("filter_level", 0x1fff)] * 5)),
                        record.get("filter_env_ad", 0), record.get("filter_env_dr", 0)),
        ("direct", "dsp_send", "lfo"), lambda record: record["total_level"])


def _compile_pcm16_records(timeline: dict, records: list[dict], mapping: dict, tick_rate: int,
                           keyoff_cluster: dict | None, kind: str, warnings: list[dict]) -> tuple[bytes, dict]:
    samples, sample_index = [], {}
    for record in records:
        identity = (hashlib.sha256(record["raw"]).digest(), record["format"], record["frames"],
                    record["loop_start"], record["loop_end"], record["loop"])
        record["sample"] = sample_index.setdefault(identity, len(samples))
        if record["sample"] == len(samples): samples.append(record)
    setups, dictionary_optimization = optimize_record_setups(records)
    events = []
    for note, record in zip(timeline["notes"], records):
        start = control_start(timeline, note, tick_rate)
        end = control_end(timeline, note, tick_rate)
        if end <= start: raise CompileError(f"note {note['id']} quantized to an empty interval")
        events.extend(((end, 0, note["local_channel"], note["id"], None),
                       (start, 1, note["local_channel"], note["id"], (note, record))))
    events.extend(timed_controller_patches(timeline, records, tick_rate))
    events.extend(timed_baked_level_patches(timeline, records, tick_rate))
    events.extend(timed_expression_patches(timeline, records, mapping, tick_rate))
    events, curve_patch_policy = spread_baked_level_patches(timeline, events, mapping, tick_rate)
    events.sort(key=lambda event: event[:4])  # KEYOFF, NOTE, then PATCH at a shared tick.
    def encode_event(event):
        _, kind, channel, _, payload = event
        if kind == 0:
            return bytes((AFX_OP_KEYOFF, channel))
        if kind >= 2:
            return encode_patch(channel, payload)
        note, record = payload
        setup = setups[record["setup"]]
        return encode_note(channel, record["setup"], record_pitch(note, record),
                           record_total_level(note, record),
                           record["direct"] if record["direct"] != setup["direct"] else None,
                           record["dsp_send"] if record["dsp_send"] != setup["dsp_send"] else None,
                           record["lfo"] if record["lfo"] != setup["lfo"] else None)
    end_tick = max((control_lifetime_end(timeline, note, tick_rate) for note in timeline["notes"]), default=0)
    event_offsets = []
    stream, batches, stream_end = build_stream(events, encode_event, end_tick, event_offsets)
    lane_map = timeline.get("lane_map")
    if lane_map is not None and len(lane_map) != timeline["allocation"]["local_channels"]:
        raise CompileError("lane map must contain one entry per local channel")
    stream_offset = len(setups) * SETUP_BYTES + (len(lane_map) if lane_map else 0)
    sample_cursor = align(stream_offset + len(stream), 32)
    sample_offsets = []
    for sample in samples:
        sample_cursor = align(sample_cursor, 32)
        sample_offsets.append(sample_cursor)
        sample_cursor += len(sample["raw"])
    image_size = sample_cursor
    require_asset_capacity(image_size)
    def checkpoint_state(payload):
        note, record = payload["note"], payload["record"]
        setup = record  # Authored state, before dictionary substitutions.
        offset = sample_offsets[setup["sample"]]
        state = [0] * 18
        state[0] = (0x0200 if setup["loop"] else 0) | (samples[setup["sample"]]["format"] << 7) | ((offset >> 16) & 0x7F)
        state[1] = offset & 0xFFFF
        state[2], state[3] = setup["loop_start"], setup["loop_end"]
        state[4], state[5] = setup["env_ad"], setup["env_dr"]
        state[6] = record_pitch(note, record)
        state[7] = setup["lfo"]
        state[8], state[9], state[10] = setup["dsp_send"], setup["direct"], record_total_level(note, record)
        state[11:16] = setup.get("filter_levels", [setup.get("filter_level", 0x1fff)] * 5)
        state[16], state[17] = setup.get("filter_env_ad", 0), setup.get("filter_env_dr", 0)
        for field, value in payload["patch"].items(): state[field] = value
        return state
    checkpoints = encode_checkpoints(checkpoint_plan(events, batches, stream_offset, stream_end),
                                     checkpoint_state)
    samples_offset = 80
    relocations_offset = samples_offset + len(samples) * 16
    checkpoints_offset = relocations_offset + len(setups) * 12
    metadata_size = checkpoints_offset + len(checkpoints)
    image_offset = align(metadata_size, 32)
    total_size = image_offset + image_size
    file = bytearray(total_size)
    work = work_report(events)
    flags = AFX_FLAG_MUSIC | (AFX_FLAG_MUSIC_CHORUS if any(record.get("midi_chorus") == "apply" for record in records) else 0)
    if lane_map: flags |= AFX_FLAG_LANES
    header = (AFX_FILE_MAGIC, AFX_ABI_VERSION, total_size, flags,
              image_offset, image_size, stream_offset, len(stream), 0, len(setups),
              samples_offset, len(samples), relocations_offset, len(setups), checkpoints_offset, len(checkpoints),
              timeline["allocation"]["local_channels"], tick_rate, 1, encode_work_profile(work))
    struct.pack_into("<20I", file, 0, *header)
    image = memoryview(file)[image_offset:]
    for index, sample in enumerate(samples):
        offset = sample_offsets[index]
        struct.pack_into("<4I", file, samples_offset + index * 16, offset, len(sample["raw"]),
                         sample["frames"], sample["format"])
        image[offset:offset + len(sample["raw"])] = sample["raw"]
    for index, setup in enumerate(setups):
        offset = sample_offsets[setup["sample"]]
        state = [0] * 18
        state[0] = (0x0200 if setup["loop"] else 0) | (samples[setup["sample"]]["format"] << 7) | ((offset >> 16) & 0x7F)
        state[1] = offset & 0xFFFF
        state[2], state[3] = setup["loop_start"], setup["loop_end"]
        state[4], state[5] = setup["env_ad"], setup["env_dr"]
        state[7] = setup["lfo"]
        state[8], state[9], state[10] = setup["dsp_send"], setup["direct"], setup["total_level"]
        state[11:16] = setup.get("filter_levels", [setup.get("filter_level", 0x1fff)] * 5)
        state[16], state[17] = setup.get("filter_env_ad", 0), setup.get("filter_env_dr", 0)
        struct.pack_into("<18H", image, index * SETUP_BYTES, *state)
        struct.pack_into("<3I", file, relocations_offset + index * 12,
                         index * SETUP_BYTES, setup["sample"], 0)
    if lane_map:
        image[len(setups) * SETUP_BYTES:stream_offset] = bytes(lane_map)
    file[checkpoints_offset:checkpoints_offset + len(checkpoints)] = checkpoints
    image[stream_offset:stream_offset + len(stream)] = stream
    if keyoff_cluster:
        warnings.append({"kind": "event_cluster_spread",
                         "detail": "explicit event-cluster policy shifts selected note starts and/or note-offs",
                         **keyoff_cluster})
    notes = len(records)
    setup_encoding = {
        "notes": notes, "setup_state_bytes": len(setups) * SETUP_BYTES,
        "relocation_bytes": len(setups) * 12,
        "note_reference_bytes": sum(len(encode_event(event)) for event in events if event[1] == 1),
        "optimization": dictionary_optimization,
        "full_note_state_bytes": notes * (8 + SETUP_BYTES),
    }
    setup_encoding["encoded_bytes"] = (setup_encoding["setup_state_bytes"] +
                                        setup_encoding["relocation_bytes"] +
                                        setup_encoding["note_reference_bytes"])
    setup_encoding["savings_bytes"] = (setup_encoding["full_note_state_bytes"] -
                                         setup_encoding["encoded_bytes"])
    diagnostics = {
        "abi": AFX_ABI_VERSION,
        "source_sha256": hashlib.sha256(json.dumps(timeline, sort_keys=True).encode()).hexdigest(),
        "kind": kind, "tick_rate": tick_rate,
        "required_channels": timeline["allocation"]["local_channels"], "setups": len(setups),
        "sample_count": len(samples),
        "sample_regions": sorted({str(record.get("name", "<unnamed>")) for record in records}),
        "pcm16_bytes": sum(len(sample["raw"]) for sample in samples if sample["format"] == PCM16),
        "pcm8_bytes": sum(len(sample["raw"]) for sample in samples if sample["format"] == PCM8),
        "adpcm_bytes": sum(len(sample["raw"]) for sample in samples if sample["format"] == ADPCM),
        "sample_bytes": sum(len(sample["raw"]) for sample in samples),
        "stream_bytes": len(stream), "checkpoint_count": len(checkpoint_plan(events, batches, stream_offset, stream_end)),
        "checkpoint_bytes": len(checkpoints), "image_bytes": image_size, "total_bytes": total_size,
        "duration_ticks": end_tick, "work": work, "keyoff_cluster": keyoff_cluster,
        "lanes": len(set(lane_map)) if lane_map else 0,
        "curve_patch_policy": curve_patch_policy,
        "setup_dictionary": setup_encoding,
        "source_events": source_event_map(timeline, events, event_offsets, stream_offset),
        "warnings": warnings + ([{"kind": "baked_level_curve_spread",
                                    "detail": "explicit policy delayed only authored curve PATCHes",
                                    **curve_patch_policy}] if curve_patch_policy else []),
    }
    return bytes(file), diagnostics


def _sf2_warnings(timeline: dict, records: list[dict], tick_rate: int) -> list[dict]:
    unlowered_controls = set()
    for record in records:
        controls = dict(record["source_controls"])
        # These are lowered per NOTE (TL) and per setup (DIPAN), respectively.
        controls["attenuation_centibels"] = controls["pan_centibels"] = None
        if record["source_reverb_applied"]:
            controls["reverb_send_per_mille"] = None
        if record["envelope_mode"] in ("aica_mame_v1", "aica_explicit_rates_v1"):
            controls["attack_timecents"] = controls["release_timecents"] = None
        if record["envelope_mode"] == "aica_sf2_adsr_v1":
            for name in ("attack_timecents", "decay_timecents", "sustain_centibels", "release_timecents"):
                controls[name] = None
        if record.get("filter_model"):
            controls["filter_cutoff_cents"] = controls["filter_q_centibels"] = None
            if record["filter_model"] == "sf2_envelope":
                for name in controls:
                    if name.startswith("mod_env_"): controls[name] = None
        if any(value is not None and value != 0 for value in controls.values()):
            unlowered_controls.add(json.dumps(controls, sort_keys=True))
    warnings = [{"kind": "fixed_envelope", "detail": "sf2_pcm16 currently uses the declared fixed AICA envelope"}]
    if any(record["envelope_mode"] == "aica_mame_v1" for record in records):
        warnings = [{"kind": "aica_envelope_approximation",
                     "detail": "SF2 attack/release use nearest KRS-off AICA rates from the MAME AICA timing model"}]
    if any(record["envelope_mode"] == "aica_explicit_rates_v1" for record in records):
        warnings = [{"kind": "aica_explicit_envelope",
                     "detail": "mapping supplies explicit KRS-off AICA rates"}]
    if any(record["envelope_mode"] == "aica_sf2_adsr_v1" for record in records):
        warnings = [{"kind": "aica_sf2_adsr_approximation",
                     "detail": "SF2 volume attack, decay and sustain lower into AICA AR/D1R/DL; declared release_rate overrides the SF2 release"}]
    if any(record.get("filter_model") for record in records):
        warnings.append({"kind": "aica_filter_approximation", "detail":
                         "SF2 cutoff/Q and selected velocity modulator use native AICA filter levels; sf2_envelope additionally approximates modulation attack/decay/release with native FEG ramps"})
    if any(not record["source_loop"] for record in records):
        warnings.append({"kind": "aica_one_shot_silence_loop",
                         "detail": "non-looping recordings fade only at their natural tail then forward-loop zero PCM to prevent AICA overread"})
    if any(note.get("tail_ticks", 0) for note in timeline["notes"]):
        warnings.append({"kind": "pcm16_release_tail",
                         "detail": "authored tail_ms reserves the local channel after key-off; seek during release is approximate"})
    if any(record["format"] == PCM8 for record in records):
        warnings.append({"kind": "pcm8_hardware_ab_required",
                         "detail": "PCM8 halves sample RAM but remains an author-selected capture A/B choice"})
    if any(record["format"] == ADPCM for record in records):
        warnings.append({"kind": "adpcm_hardware_ab_required",
                         "detail": "ADPCM quarters sample RAM but its encoder and loop behavior require capture A/B"})
    if unlowered_controls:
        warnings.append({"kind": "unlowered_sf2_controls",
                         "detail": "unmapped SF2 controls are reported rather than silently discarded",
                         "settings": [json.loads(value) for value in sorted(unlowered_controls)]})
    attack_conflicts = []
    for note, record in zip(timeline["notes"], records):
        attack = record["source_controls"]["attack_timecents"]
        if attack is None: continue
        duration = tick_to_control_ticks(timeline, note["end_tick"], tick_rate) - \
                   tick_to_control_ticks(timeline, note["start_tick"], tick_rate)
        source_attack_ms = round(1000 * 2 ** (attack / 1200))
        if duration < source_attack_ms:
            attack_conflicts.append((duration, source_attack_ms))
    if attack_conflicts:
        modes = sorted({record["envelope_mode"] for record in records})
        if modes == ["aica_explicit_rates_v1"]:
            detail = ("SF2 declared attack exceeds authored note duration; the explicit AICA attack is active, "
                      "but audition the source transient")
        elif modes == ["fixed"]:
            detail = "SF2 declared attack exceeds authored note duration; choose an explicit envelope policy"
        else:
            detail = ("SF2 declared attack exceeds authored note duration; the selected source-derived envelope "
                      "may need a baked or explicit transient policy")
        warnings.append({"kind": "source_attack_exceeds_note",
                         "detail": detail, "envelope_modes": modes,
                         "notes": len(attack_conflicts),
                         "shortest_note_ms": min(item[0] for item in attack_conflicts),
                         "source_attack_ms": max(item[1] for item in attack_conflicts)})
    return warnings


def compile_sf2_pcm16_timeline(timeline: dict, mapping: dict, tick_rate: int,
                               keyoff_cluster: dict | None) -> tuple[bytes, dict]:
    records = _sf2_records(timeline, mapping)
    return _compile_pcm16_records(timeline, records, mapping, tick_rate, keyoff_cluster, "sf2_pcm16",
                                  _sf2_warnings(timeline, records, tick_rate))


def _raw_pcm16_records(timeline: dict, mapping: dict, kind: str = "raw_pcm16") -> list[dict]:
    """Choose one explicit PCM zone per MIDI note, optionally as a looped cycle."""
    instruments = mapping.get("instruments")
    if not isinstance(instruments, dict):
        raise CompileError("mapping must contain an instruments object")
    needed = sorted({mapping_key(note) for note in timeline["notes"]})
    missing = [key for key in needed if key not in instruments]
    if missing:
        available = ", ".join(sorted(instruments)) or "none"
        raise CompileError(f"missing instrument mappings: {', '.join(missing)}; available: {available}")
    if any(instrument_config(mapping, note).get("kind") != kind for note in timeline["notes"]):
        raise CompileError(f"{kind} compile cannot mix instrument kinds")
    records = []
    for note in timeline["notes"]:
        key, config = mapping_key(note), instrument_config(mapping, note)
        if config.get("velocity") not in ("fixed", "zones", "midi") or config.get("release") != "keyoff":
            raise CompileError(f"{key}: {kind} requires velocity=fixed, zones or midi and release=keyoff")
        zones = config.get("zones")
        if not isinstance(zones, list) or not zones:
            raise CompileError(f"{key}: {kind} requires non-empty zones")
        try:
            selected = [zone for zone in zones if isinstance(zone, dict) and
                        int(zone.get("key_min", 0)) <= note["key"] <= int(zone.get("key_max", 127)) and
                        (config["velocity"] != "zones" or int(zone.get("velocity_min", 0)) <= note["velocity"] <=
                         int(zone.get("velocity_max", 127)))]
        except (TypeError, ValueError) as error:
            raise CompileError(f"{key}: {kind} zone key/velocity ranges must be integers") from error
        if len(selected) != 1:
            raise CompileError(f"note {note['id']}: {kind} needs exactly one zone for key {note['key']} "
                               f"at velocity {note['velocity']}; found {len(selected)}")
        zone = selected[0]
        raw = zone.get("pcm16_bytes")
        source_rate, rate = zone.get("source_rate"), zone.get("resample_hz")
        if not isinstance(raw, bytes) or not raw or len(raw) & 1:
            raise CompileError(f"{key}: {kind} zone must contain non-empty little-endian PCM16 bytes")
        if not isinstance(source_rate, int) or not isinstance(rate, int) or not 4000 <= rate <= source_rate:
            raise CompileError(f"{key}: {kind} zone requires source_rate and resample_hz in 4000..source_rate")
        raw = resample_pcm16(raw, source_rate, rate)
        frames = len(raw) // 2
        if frames > 65535:
            raise CompileError(f"{key}: {kind} has {frames} frames after resampling; trim it or choose a lower resample_hz")
        if kind == "wavetable":
            try:
                source_start, source_end = int(zone["loop_start"]), int(zone["loop_end"])
            except (KeyError, TypeError, ValueError) as error:
                raise CompileError(f"{key}: wavetable zone requires integer loop_start and exclusive loop_end") from error
            loop_start = round(source_start * rate / source_rate)
            loop_end = round(source_end * rate / source_rate)
            if not 0 <= loop_start < loop_end <= frames:
                raise CompileError(f"{key}: wavetable loop must lie inside its source frames")
            loop_end -= 1  # AICA LEA is inclusive; authoring loop_end is exclusive.
            loop = True
        else:
            loop_start, loop_end, loop = 0, frames - 1, False
        controls = {"attack_timecents": None, "decay_timecents": None,
                    "sustain_centibels": None, "release_timecents": None}
        env_ad, env_dr = aica_envelope(controls, "aica_sf2_adsr_v1", config)
        sample_format = configured_sample_format(config, key)
        if sample_format == PCM8: raw = pcm16_to_pcm8(raw)
        if sample_format == ADPCM: raw = pcm16_to_adpcm(raw)
        record = {"raw": raw, "format": sample_format, "frames": frames, "loop_start": loop_start, "loop_end": loop_end,
                        "loop": loop, "root_key": int(zone.get("root_key", note["key"])),
                        "tuning_cents": int(zone.get("tuning_cents", 0)),
                        # AICA advances PCM against its native 44.1 kHz output clock,
                        # not against the file's original conversion rate.
                        "rate_tuning_cents": aica_rate_tuning_cents(rate),
                        "env_ad": env_ad, "env_dr": env_dr, "lfo": aica_triangle_lfo(config),
                        "lfo_config": config.get("lfo"), "midi_modulation": midi_modulation_policy(config),
                        "base_attenuation": float(config.get("attenuation_centibels", 0)),
                        "total_level": aica_total_level(float(config.get("attenuation_centibels", 0)), int(config.get("mix", 0x24)) & 0xff),
                        "lpf": int(config.get("mix", 0x24)) & 0xff,
                        "dsp_send": aica_dsp_send(config), "dsp_config": config,
                        "midi_reverb": midi_reverb_policy(config), "midi_chorus": midi_chorus_policy(config),
                        "midi_filter": midi_filter_policy(config),
                        "direct_config": config,
                        "direct": aica_direct(config, None, note.get("channel_pan", 64)),
                        "baked_level_curve": baked_level_curve(config),
                        "name": str(zone.get("name", "<wavetable>" if loop else "<raw PCM16>"))}
        record["dsp_send"] = record_dsp_send(note, record)
        record["lfo"] = record_lfo(note, record)
        record["direct"] = record_direct(record, note)
        if config["velocity"] == "midi":
            record["velocity_attenuation"] = sf2_velocity_attenuation_centibels(note["velocity"])
        record["total_level"] = record_total_level(note, record)
        records.append(record)
    return records


def compile_raw_pcm16_timeline(timeline: dict, mapping: dict, tick_rate: int,
                               keyoff_cluster: dict | None) -> tuple[bytes, dict]:
    records = _raw_pcm16_records(timeline, mapping)
    velocities = {value.get("velocity") for value in mapping.get("instruments", {}).values()
                  if isinstance(value, dict)}
    warnings = [{"kind": "raw_pcm16_one_shot",
                 "detail": ("non-looping recorded PCM zones retain their source decay; explicit velocity zones select attacks"
                            if velocities == {"zones"} else
                            "non-looping recorded PCM zones retain their source decay; mapping uses a fixed velocity layer")}]
    if any(note.get("tail_ticks", 0) for note in timeline["notes"]):
        warnings.append({"kind": "raw_pcm16_release_tail",
                         "detail": "authored tail_ms reserves the local channel and delays END; seek during release is approximate"})
    if any(record["format"] == PCM8 for record in records):
        warnings.append({"kind": "pcm8_hardware_ab_required",
                         "detail": "PCM8 halves sample RAM but remains an author-selected capture A/B choice"})
    if any(record["format"] == ADPCM for record in records):
        warnings.append({"kind": "adpcm_hardware_ab_required",
                         "detail": "ADPCM quarters sample RAM but its encoder and loop behavior require capture A/B"})
    return _compile_pcm16_records(timeline, records, mapping, tick_rate, keyoff_cluster, "raw_pcm16", warnings)


def compile_wavetable_timeline(timeline: dict, mapping: dict, tick_rate: int,
                               keyoff_cluster: dict | None) -> tuple[bytes, dict]:
    """Lower explicit revision-owned waveform cycles through the normal PCM path."""
    records = _raw_pcm16_records(timeline, mapping, "wavetable")
    warnings = [{"kind": "wavetable_native_loop",
                 "detail": "short PCM cycles use AICA sample looping and native pitch; no ARM7 oscillator is involved"}]
    if any(note.get("tail_ticks", 0) for note in timeline["notes"]):
        warnings.append({"kind": "wavetable_release_tail",
                         "detail": "authored tail_ms reserves the local channel after key-off; seek during release is approximate"})
    if any(record["format"] == PCM8 for record in records):
        warnings.append({"kind": "pcm8_hardware_ab_required",
                         "detail": "PCM8 halves wavetable RAM but remains an author-selected capture A/B choice"})
    if any(record["format"] == ADPCM for record in records):
        warnings.append({"kind": "adpcm_hardware_ab_required",
                         "detail": "ADPCM wavetable loop behaviour requires capture A/B"})
    return _compile_pcm16_records(timeline, records, mapping, tick_rate, keyoff_cluster, "wavetable", warnings)


def attach_source_warnings(result: tuple[bytes, dict], warnings: list[dict]) -> tuple[bytes, dict]:
    """Keep best-effort MIDI feature diagnostics on the compiled candidate."""
    image, diagnostics = result
    if warnings:
        diagnostics["warnings"] = warnings + diagnostics["warnings"]
    return image, diagnostics


CURVE_LIMITS = {"gain_db": (-24, 24), "vibrato_depth": (0, 7), "vibrato_rate": (0, 31), "shorten_ms": (0, 30)}


def expression_curves(config):
    curves = config.get("expression_curves") or {}
    if not isinstance(curves, dict) or set(curves) - CURVE_LIMITS.keys():
        raise CompileError("unknown expression curve")
    for field, points in curves.items():
        low, high = CURVE_LIMITS[field]
        if not isinstance(points, list) or not 2 <= len(points) <= 64:
            raise CompileError("expression curves need 2..64 points")
        previous = -1
        for point in points:
            if not isinstance(point, list) or len(point) != 2:
                raise CompileError("curve points are [song_ms, value]")
            when, value = point
            if type(when) is not int or not previous < when <= 3600000 or type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
                raise CompileError(f"invalid {field} curve point")
            previous = when
    return curves


def curve_value(points, when):
    if when <= points[0][0]: return points[0][1]
    for (a, x), (b, y) in zip(points, points[1:]):
        if when <= b: return x + (y - x) * (when - a) / (b - a)
    return points[-1][1]


def prepare_expression_curves(timeline, mapping, tick_rate):
    import copy
    if not any(expression_curves(instrument_config(mapping, note)) for note in timeline["notes"]):
        return mapping
    mapping = copy.deepcopy(mapping)
    for note in timeline["notes"]:
        config = instrument_config(mapping, note)
        curves = expression_curves(config)
        if not curves: continue
        when = control_start(timeline, note, tick_rate) * 1000 / tick_rate
        patch = {}
        if "gain_db" in curves: patch["attenuation_centibels"] = config.get("attenuation_centibels", 0) - 100 * curve_value(curves["gain_db"], when)
        if "shorten_ms" in curves: patch["performance"] = {**(config.get("performance") or {}), "shorten_ms": round(curve_value(curves["shorten_ms"], when))}
        if "vibrato_depth" in curves or "vibrato_rate" in curves:
            patch["lfo"] = {"rate": 16, "pitch_depth": 0, "amplitude_depth": 0, **(config.get("lfo") or {})}
            for field, target in (("vibrato_depth", "pitch_depth"), ("vibrato_rate", "rate")):
                if field in curves: patch["lfo"][target] = round(curve_value(curves[field], when))
        key = str(note.get("source_note_id", note["id"]))
        overrides = mapping.setdefault("note_overrides", {})
        overrides[key] = {**overrides.get(key, {}), **patch}
    return mapping


def timed_expression_patches(timeline, records, mapping, tick_rate):
    result = []
    for note, record in zip(timeline["notes"], records):
        curves = expression_curves(instrument_config(mapping, note))
        if not set(curves) - {"shorten_ms"}: continue
        start, end = control_start(timeline, note, tick_rate), control_end(timeline, note, tick_rate)
        controller_ticks = {tick_to_control_ticks(timeline, event["tick"], tick_rate)
                            for event in timeline.get("automation", []) if event["channel"] == note["channel"]}
        times = set(range(start + max(1, round(tick_rate / 10)), end, max(1, round(tick_rate / 10))))
        times.update(round(point[0] * tick_rate / 1000) for points in curves.values() for point in points)
        times.update(controller_ticks)
        initial_gain = curve_value(curves["gain_db"], start * 1000 / tick_rate) if "gain_db" in curves else 0
        previous = {}
        if "gain_db" in curves: previous[AFX_FIELD_TOTAL_LEVEL] = record_total_level(note, record)
        if "vibrato_depth" in curves or "vibrato_rate" in curves: previous[AFX_FIELD_LFO] = record_lfo(note, record)
        for when in sorted(t for t in times if start < t < end):
            ms = when * 1000 / tick_rate
            controller = controller_state_at(timeline, note, tick_rate, when)
            fields = {}
            if "gain_db" in curves:
                fields[AFX_FIELD_TOTAL_LEVEL] = record_total_level(note, record, controller, 100 * (initial_gain - curve_value(curves["gain_db"], ms)))
            if "vibrato_depth" in curves or "vibrato_rate" in curves:
                value = record_lfo(note, record, controller)
                for field, shift, mask in (("vibrato_depth", 5, 7), ("vibrato_rate", 10, 31)):
                    if field in curves: value = (value & ~(mask << shift)) | (round(curve_value(curves[field], ms)) << shift)
                fields[AFX_FIELD_LFO] = value
            changed = {key: value for key, value in fields.items() if previous.get(key) != value or when in controller_ticks}
            if changed: result.append((when, 4, note["local_channel"], note["id"], changed))
            previous = fields
    return result


def scoped_expression(timeline, mapping, tick_rate):
    import copy
    if not any(instrument_config(mapping, n).get("performance") for n in timeline["notes"]):
        return timeline, mapping
    timeline, mapping = copy.deepcopy(timeline), copy.deepcopy(mapping)
    for note in timeline["notes"]:
        config = instrument_config(mapping, note)
        performance = config.get("performance")
        if not performance: continue
        if not isinstance(performance, dict) or set(performance) - {"variation", "shorten_ms", "seed"}:
            raise CompileError("performance needs variation, shorten_ms and seed")
        for key, upper in (("variation", 100), ("shorten_ms", 30), ("seed", 0xffffffff)):
            value = performance.get(key, 0)
            if type(value) is not int or not 0 <= value <= upper:
                raise CompileError(f"performance {key} must be an integer in 0..{upper}")
        amount = performance.get("variation", 0)
        identity = [performance.get("seed", 0), note["id"], note["track"], note["start_tick"], note["key"]]
        digest = hashlib.sha256(json.dumps(identity).encode()).digest()
        extent = round(amount * .8)
        patch = {}
        if extent:
            patch["attenuation_centibels"] = config.get("attenuation_centibels", 0) + int.from_bytes(digest[:4], "little") % (2 * extent + 1) - extent
        start, end = control_start(timeline, note, tick_rate), control_end(timeline, note, tick_rate)
        shorten = performance.get("shorten_ms", 0) + digest[4] / 255 * 16 * amount / 100
        set_control_end(note, end - min(round(shorten * tick_rate / 1000), max(0, (end - start) // 20)))
        if config.get("lfo") and digest[5] < amount * 2.55:
            patch["lfo"] = {**config["lfo"], "rate": max(0, min(31, config["lfo"]["rate"] + (1 if digest[6] & 1 else -1)))}
        key = str(note.get("source_note_id", note["id"]))
        overrides = mapping.setdefault("note_overrides", {})
        overrides[key] = {**overrides.get(key, {}), **patch}
    return timeline, mapping


def compile_timeline(timeline: dict, mapping: dict, tick_rate: int = 1000, metadata: dict | None = None) -> tuple[bytes, dict]:
    from afx_metadata import attach
    def identity(value):
        def encode(item):
            if isinstance(item, bytes): return {"sha256": hashlib.sha256(item).hexdigest()}
            if isinstance(item, Path): return str(item)
            raise TypeError(type(item).__name__)
        return hashlib.sha256(json.dumps(value, sort_keys=True, default=encode, separators=(",", ":")).encode()).hexdigest()
    recipes = {json.dumps(config["performance"], sort_keys=True) for field in
               ("instruments", "track_overrides", "source_overrides", "note_overrides")
               for config in (mapping.get(field, {}) if isinstance(mapping.get(field, {}), dict) else {}).values() if isinstance(config, dict) and config.get("performance")}
    info = {"title": mapping.get("title", "Untitled"),
            "instruments": {key: config.get("label", key) for key, config in (mapping.get("instruments", {}) if isinstance(mapping.get("instruments", {}), dict) else {}).items() if isinstance(config, dict)},
            "source_timeline_sha256": identity(timeline), "settings_sha256": identity(mapping),
            "compiler": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                         for name in ("afx_compile.py", "afx_midi.py", "afx_sf2.py", "afx_metadata.py")},
            "expression": {"algorithm": "scoped_expression_sha256_v1", "recipes": [json.loads(r) for r in sorted(recipes)]},
            **(metadata or {})}
    image, diagnostics = _compile_performance(timeline, mapping, tick_rate)
    if diagnostics.get("humanize"): info["humanization"] = diagnostics["humanize"]
    image = attach(image, info)
    diagnostics.update(total_bytes=len(image), container_version=1, package_metadata=info)
    return image, diagnostics


def _compile_performance(timeline: dict, mapping: dict, tick_rate: int = 1000) -> tuple[bytes, dict]:
    """Optional deterministic expression, baked before layer expansion/allocation."""
    if type(tick_rate) is not int or not 1 <= tick_rate <= 1_000_000:
        raise CompileError("tick rate must be in 1..1000000")
    unresolved = [warning for warning in timeline["warnings"]
                  if not warning["kind"].startswith("unsupported_")]
    if unresolved:
        raise CompileError(f"source timeline has unresolved warnings: {unresolved}")
    instruments = mapping.get("instruments", {})
    if not isinstance(instruments, dict):
        raise CompileError("instruments must be an object")
    missing = sorted({mapping_key(note) for note in timeline["notes"]} - instruments.keys())
    if missing:
        raise CompileError(f"missing instrument mappings: {', '.join(missing)}; available: {', '.join(sorted(instruments)) or 'none'}")
    mapping = prepare_expression_curves(timeline, mapping, tick_rate)
    timeline, mapping = scoped_expression(timeline, mapping, tick_rate)
    settings = mapping.get("humanize")
    if settings is None:
        return _compile_timeline(timeline, mapping, tick_rate)
    if not isinstance(settings, dict) or set(settings) - {"seed", "level_centibels", "shorten_ms", "lfo_rate_steps"}:
        raise CompileError("humanize needs seed and optional level_centibels, shorten_ms, lfo_rate_steps")
    limits = {"seed": (0, 0xffffffff, None), "level_centibels": (0, 120, 40),
              "shorten_ms": (0, 20, 8), "lfo_rate_steps": (0, 1, 1)}
    opts = {}
    for name, (low, high, default) in limits.items():
        value = settings.get(name, default)
        if type(value) is not int or not low <= value <= high:
            raise CompileError(f"humanize {name} must be an integer in {low}..{high}")
        opts[name] = value
    if type(tick_rate) is not int or tick_rate <= 0:
        raise CompileError("tick rate must be positive")
    import copy
    timeline, mapping = copy.deepcopy(timeline), copy.deepcopy(mapping)
    overrides = mapping.setdefault("note_overrides", {})
    for note in timeline["notes"]:
        if note["channel"] == 9:
            continue
        config = instrument_config(mapping, note)
        identity = [opts["seed"], note["id"], note["track"], note["start_tick"], note["key"]]
        digest = hashlib.sha256(json.dumps(identity, separators=(",", ":")).encode()).digest()
        def variation(offset, extent):
            return int.from_bytes(digest[offset:offset + 4], "little") % (2 * extent + 1) - extent
        change = {}
        if opts["level_centibels"]:
            change["attenuation_centibels"] = max(0, config.get("attenuation_centibels", 0) + variation(0, opts["level_centibels"]))
        start, end = control_start(timeline, note, tick_rate), control_end(timeline, note, tick_rate)
        maximum = min(round(opts["shorten_ms"] * tick_rate / 1000), max(0, (end - start) // 20))
        if maximum:
            set_control_end(note, end - int.from_bytes(digest[4:8], "little") % (maximum + 1))
        if opts["lfo_rate_steps"] and config.get("lfo"):
            change["lfo"] = {**config["lfo"], "rate": max(0, min(31, config["lfo"]["rate"] + variation(8, opts["lfo_rate_steps"])))}
        key = str(note.get("source_note_id", note["id"]))
        overrides[key] = {**overrides.get(key, {}), **change}
    image, diagnostics = _compile_timeline(timeline, mapping, tick_rate)
    diagnostics["humanize"] = {"algorithm": "expression_sha256_v1", **opts}
    return image, diagnostics


def _compile_timeline(timeline: dict, mapping: dict, tick_rate: int = 1000) -> tuple[bytes, dict]:
    kinds = {instrument_config(mapping, note).get("kind") for note in timeline["notes"]}
    pcm_kinds = {"raw_pcm16", "sf2_pcm16", "wavetable"}
    mixed = len(kinds) > 1 and kinds <= pcm_kinds
    if "sf2_pcm16" in kinds and (mixed or kinds == {"sf2_pcm16"}):
        if mixed:
            sampled = {**timeline, "notes": [note for note in timeline["notes"]
                        if instrument_config(mapping, note)["kind"] == "sf2_pcm16"]}
            plain = [{**note, "source_note_id": note["id"]} for note in timeline["notes"]
                     if instrument_config(mapping, note)["kind"] != "sf2_pcm16"]
            expand_sf2_layers(sampled, mapping)
            timeline["notes"] = sorted(plain + sampled["notes"], key=lambda note:
                (note["start_tick"], note["track"], note["order"], note["source_note_id"], note.get("_sf2_layer", 0)))
            for index, note in enumerate(timeline["notes"]): note["id"] = index
        else:
            expand_sf2_layers(timeline, mapping)
    if kinds and kinds <= pcm_kinds:
        prepare_pcm16_tails(timeline, mapping, tick_rate)
    if timeline["allocation"]["local_channels"] > 64:
        raise CompileError(f"voice budget exhausted: {timeline['allocation']['local_channels']} local channels; AICA has 64")
    keyoff_cluster = spread_keyoff_clusters(timeline, mapping, tick_rate)
    event_cluster = spread_event_clusters(timeline, mapping, tick_rate)
    cluster_policy = event_cluster or keyoff_cluster
    if timeline["allocation"]["local_channels"] > 64:
        raise CompileError(f"voice budget exhausted after keyoff spreading: "
                           f"{timeline['allocation']['local_channels']} local channels; AICA has 64")
    if mixed:
        records_by_id, warnings = {}, list(timeline["warnings"])
        for kind in sorted(kinds):
            part = {**timeline, "notes": [note for note in timeline["notes"]
                     if instrument_config(mapping, note)["kind"] == kind]}
            records = (_sf2_records(part, mapping) if kind == "sf2_pcm16" else
                       _raw_pcm16_records(part, mapping, kind))
            records_by_id.update((note["id"], record) for note, record in zip(part["notes"], records))
            warnings.extend(_sf2_warnings(part, records, tick_rate) if kind == "sf2_pcm16" else
                            [{"kind": "explicit_" + kind, "detail": "Author-selected sample strategy in a mixed instrument flow"}])
        return _compile_pcm16_records(timeline, [records_by_id[note["id"]] for note in timeline["notes"]],
                                      mapping, tick_rate, cluster_policy, "mixed_pcm", warnings)
    if kinds == {"sf2_pcm16"}:
        return attach_source_warnings(compile_sf2_pcm16_timeline(timeline, mapping, tick_rate, cluster_policy),
                                      timeline["warnings"])
    if kinds == {"raw_pcm16"}:
        return attach_source_warnings(compile_raw_pcm16_timeline(timeline, mapping, tick_rate, cluster_policy),
                                      timeline["warnings"])
    if kinds == {"wavetable"}:
        return attach_source_warnings(compile_wavetable_timeline(timeline, mapping, tick_rate, cluster_policy),
                                      timeline["warnings"])
    instruments, warnings = resolve_instruments(timeline, mapping)

    setups, setup_index = [], {}
    for key, instrument in instruments.items():
        encoded = json.dumps(instrument, sort_keys=True, separators=(",", ":"))
        if encoded not in setup_index:
            setup_index[encoded] = len(setups)
            setups.append(instrument)
        instruments[key]["setup"] = setup_index[encoded]

    sine_records = []
    for note in timeline["notes"]:
        config = instrument_config(mapping, note)
        record = {"base_attenuation": float(config.get("attenuation_centibels", 0)),
                  "lpf": int(config.get("mix", 0x24)) & 0xff, "direct_config": config,
                  "dsp_send": aica_dsp_send(config), "dsp_config": config,
                  "midi_reverb": midi_reverb_policy(config), "midi_chorus": midi_chorus_policy(config),
                  "midi_filter": midi_filter_policy(config),
                  "root_key": int(config.get("root_key", 69)), "tuning_cents": 0,
                  "rate_tuning_cents": 0, "lfo": aica_triangle_lfo(config),
                  "lfo_config": config.get("lfo"), "midi_modulation": midi_modulation_policy(config),
                  "baked_level_curve": baked_level_curve(config)}
        record["dsp_send"] = record_dsp_send(note, record)
        record["lfo"] = record_lfo(note, record)
        record["direct"] = record_direct(record, note)
        sine_records.append(record)
    events = []
    for note, record in zip(timeline["notes"], sine_records):
        start = control_start(timeline, note, tick_rate)
        end = control_end(timeline, note, tick_rate)
        if end <= start:
            raise CompileError(f"note {note['id']} quantized to an empty interval")
        events.append((end, 0, note["local_channel"], note["id"], None))
        events.append((start, 1, note["local_channel"], note["id"], (note, record)))
    events.extend(timed_controller_patches(timeline, sine_records, tick_rate))
    events.extend(timed_baked_level_patches(timeline, sine_records, tick_rate))
    events.extend(timed_expression_patches(timeline, sine_records, mapping, tick_rate))
    events, curve_patch_policy = spread_baked_level_patches(timeline, events, mapping, tick_rate)
    events.sort(key=lambda event: event[:4])  # KEYOFF before NOTE at a shared tick.
    event_offsets = []
    stream, previous, note_bytes = bytearray(), 0, 0
    for when, kind, channel, _, payload in events:
        stream += encode_wait(when - previous)
        previous = when
        event_offsets.append(len(stream))
        if kind == 0:
            stream += bytes((AFX_OP_KEYOFF, channel))
        elif kind >= 2:
            stream += encode_patch(channel, payload)
        else:
            note, record = payload
            instrument = instruments[mapping_key(note)]
            encoded_note = encode_note(channel, instrument["setup"],
                                  record_pitch(note, record),
                                  record_total_level(note, record),
                                  record_direct(record, note) if record_direct(record, note) != instrument["direct"] else None,
                                  record_dsp_send(note, record) if record_dsp_send(note, record) != instrument["dsp_send"] else None,
                                  record_lfo(note, record) if record_lfo(note, record) != instrument["lfo"] else None)
            stream += encoded_note
            note_bytes += len(encoded_note)
    stream += bytes((AFX_OP_END,))

    sample = sine_sample()
    lane_map = timeline.get("lane_map")
    if lane_map is not None and len(lane_map) != timeline["allocation"]["local_channels"]:
        raise CompileError("lane map must contain one entry per local channel")
    stream_offset = len(setups) * SETUP_BYTES + (len(lane_map) if lane_map else 0)
    sample_offset = align(stream_offset + len(stream), 32)
    image_size = sample_offset + len(sample)
    require_asset_capacity(image_size)
    metadata_size = 96 + len(setups) * 12
    image_offset = align(metadata_size, 32)
    total_size = image_offset + image_size
    file = bytearray(total_size)
    work = work_report(events)
    flags = AFX_FLAG_MUSIC | (AFX_FLAG_MUSIC_CHORUS if any(record.get("midi_chorus") == "apply" for record in sine_records) else 0)
    if lane_map: flags |= AFX_FLAG_LANES
    header = (AFX_FILE_MAGIC, AFX_ABI_VERSION, total_size, flags, image_offset, image_size,
              stream_offset, len(stream), 0, len(setups), 80, 1, 96, len(setups),
              0, 0, timeline["allocation"]["local_channels"], tick_rate, 1, encode_work_profile(work))
    struct.pack_into("<20I", file, 0, *header)
    struct.pack_into("<4I", file, 80, sample_offset, len(sample), 104, PCM16)
    image = memoryview(file)[image_offset:]
    for index, instrument in enumerate(setups):
        state = [0] * 18
        state[0] = 0x0200 | ((sample_offset >> 16) & 0x7F)  # native loop + sample address high
        state[1] = sample_offset & 0xFFFF
        state[3] = 100  # 100 frame period, four guard frames in the sample.
        state[4] = state[5] = 0x001F
        state[7] = instrument["lfo"]
        state[8], state[9], state[10] = instrument["dsp_send"], instrument["direct"], instrument["total_level"]
        state[11:16] = [0x1fff] * 5  # Open filter, as in sampled instruments; zero mutes physical AICA.
        struct.pack_into("<18H", image, index * SETUP_BYTES, *state)
        struct.pack_into("<3I", file, 96 + index * 12, index * SETUP_BYTES, 0, 0)
    if lane_map:
        image[len(setups) * SETUP_BYTES:stream_offset] = bytes(lane_map)
    image[stream_offset:stream_offset + len(stream)] = stream
    image[sample_offset:sample_offset + len(sample)] = sample
    notes = len(timeline["notes"])
    setup_encoding = {
        "notes": notes, "setup_state_bytes": len(setups) * SETUP_BYTES,
        "relocation_bytes": len(setups) * 12,
        "note_reference_bytes": note_bytes,
        "full_note_state_bytes": notes * (8 + SETUP_BYTES),
    }
    setup_encoding["encoded_bytes"] = (setup_encoding["setup_state_bytes"] +
                                        setup_encoding["relocation_bytes"] +
                                        setup_encoding["note_reference_bytes"])
    setup_encoding["savings_bytes"] = (setup_encoding["full_note_state_bytes"] -
                                         setup_encoding["encoded_bytes"])
    diagnostics = {
        "abi": AFX_ABI_VERSION, "source_sha256": hashlib.sha256(json.dumps(timeline, sort_keys=True).encode()).hexdigest(),
        "tick_rate": tick_rate, "required_channels": timeline["allocation"]["local_channels"],
        "setups": len(setups), "stream_bytes": len(stream), "image_bytes": image_size,
        "total_bytes": total_size, "duration_ticks": events[-1][0] if events else 0,
        "work": work, "keyoff_cluster": cluster_policy, "curve_patch_policy": curve_patch_policy,
        "setup_dictionary": setup_encoding,
        "source_events": source_event_map(timeline, events, event_offsets, stream_offset),
        "warnings": warnings + ([{"kind": "event_cluster_spread",
                                    "detail": "explicit event-cluster policy shifts selected note starts and/or note-offs",
                                    **cluster_policy}] if cluster_policy else []) +
                    ([{"kind": "baked_level_curve_spread",
                       "detail": "explicit policy delayed only authored curve PATCHes",
                       **curve_patch_policy}] if curve_patch_policy else []),
    }
    return attach_source_warnings((bytes(file), diagnostics), timeline["warnings"])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("mapping", type=Path, help="JSON object with an instruments map")
    parser.add_argument("output", type=Path)
    parser.add_argument("--tick-rate", type=int, default=1000)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        mapping = resolve_mapping_paths(json.loads(args.mapping.read_text()), args.mapping.parent)
        image, diagnostics = compile_timeline(afx_midi.parse(args.source), mapping, args.tick_rate)
        args.output.write_bytes(image)
    except (OSError, ValueError, CompileError) as error:
        print(f"afx-compile: {error}", file=sys.stderr)
        return 2
    print(json.dumps(diagnostics, sort_keys=True) if args.json else f"wrote {args.output}: {diagnostics}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
