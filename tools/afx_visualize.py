#!/usr/bin/env python3
"""Build a 60 Hz visualizer sidecar from the timed events in an AFX asset."""
import argparse
import math
import struct
from pathlib import Path

import afx_metadata

MAGIC = b"VIZ1"
VERSION = 1
BANDS = 32
RATE = 60
NOTE_HALF_LIFE_SECONDS = 0.6
HEADER = struct.Struct("<4sBBBBI")
AFX_HEADER = struct.Struct("<20I")
FIELDS = 18
SETUP_BYTES = FIELDS * 2
AFX_MAGIC = 0x32584641
WAIT8, WAIT16, WAIT32 = 1, 2, 3
NOTE, PATCH, KEYOFF, NOTE_PL, PATCH_LEVEL = 0x10, 0x11, 0x12, 0x14, 0x15
PITCH, TOTAL_LEVEL = 6, 10


def words(data: bytes, offset: int, count: int) -> list[int]:
    return list(struct.unpack_from(f"<{count}H", data, offset))


def event(data: bytes, cursor: int) -> tuple[int, int, int | None, int | None, int, list[int]]:
    """Decode one AFX wire event as (opcode, bytes, channel, setup, mask, values)."""
    if cursor >= len(data):
        raise ValueError("truncated AFX stream")
    opcode = data[cursor]
    if opcode in (0, 0x13): return opcode, 1, None, None, 0, []
    if opcode == WAIT8: return opcode, 2, None, None, data[cursor + 1], []
    if opcode == WAIT16: return opcode, 3, None, None, struct.unpack_from("<H", data, cursor + 1)[0], []
    if opcode == WAIT32: return opcode, 5, None, None, struct.unpack_from("<I", data, cursor + 1)[0], []
    if opcode == KEYOFF: return opcode, 2, data[cursor + 1], None, 0, []
    if opcode == NOTE_PL:
        return opcode, 8, data[cursor + 1], struct.unpack_from("<H", data, cursor + 2)[0], \
            (1 << PITCH) | (1 << TOTAL_LEVEL), words(data, cursor + 4, 2)
    if opcode == PATCH_LEVEL:
        return opcode, 4, data[cursor + 1], None, 1 << TOTAL_LEVEL, words(data, cursor + 2, 1)
    if opcode in (NOTE, PATCH):
        prefix = 8 if opcode == NOTE else 6
        channel = data[cursor + 1]
        setup = struct.unpack_from("<H", data, cursor + 2)[0] if opcode == NOTE else None
        mask = struct.unpack_from("<I", data, cursor + prefix - 4)[0]
        return opcode, prefix + 2 * mask.bit_count(), channel, setup, mask, words(data, cursor + prefix, mask.bit_count())
    raise ValueError(f"unsupported AFX opcode {opcode:#x}")


def decode(path: Path) -> tuple[list[tuple[int, tuple]], int, int, int, list[list[int]]]:
    data = path.read_bytes()
    if len(data) < AFX_HEADER.size: raise ValueError("truncated AFX header")
    (magic, version, total, _, image_offset, image_size, stream_offset, stream_size, control_id,
     setup_count, *_, rate_num, rate_den, _) = AFX_HEADER.unpack_from(data)
    if magic != AFX_MAGIC or total != len(data) or image_offset + image_size != len(data):
        raise ValueError("invalid AFX header")
    image = data[image_offset:]
    setups_offset = 0 if version == 7 else control_id
    if stream_offset + stream_size > len(image) or setups_offset + setup_count * SETUP_BYTES > len(image):
        raise ValueError("invalid AFX image bounds")
    setups = [words(image, setups_offset + i * SETUP_BYTES, FIELDS) for i in range(setup_count)]
    actions, tick, cursor, end = [], 0, stream_offset, stream_offset + stream_size
    while cursor < end:
        opcode, size, channel, setup, mask, values = event(image, cursor)
        if cursor + size > end: raise ValueError("invalid AFX event bounds")
        if opcode in (NOTE, PATCH, NOTE_PL, PATCH_LEVEL) and mask >> FIELDS:
            raise ValueError("invalid AFX event mask")
        cursor += size
        if opcode in (WAIT8, WAIT16, WAIT32): tick += mask
        elif opcode in (NOTE, NOTE_PL, PATCH, PATCH_LEVEL, KEYOFF):
            if channel is None or channel >= 64: raise ValueError("invalid AFX channel")
            if setup is not None and setup >= len(setups): raise ValueError("invalid AFX setup")
            actions.append((tick, (opcode, channel, setup, mask, values)))
        elif opcode in (0, 0x13):
            if cursor != end: raise ValueError("AFX terminal event is not final")
            break
    return actions, tick, rate_num, rate_den, setups


def apply(state: list[int], mask: int, values: list[int]) -> None:
    for field in range(FIELDS):
        if mask & (1 << field): state[field] = values.pop(0)


def frequency(state: list[int]) -> float:
    pitch = state[PITCH]
    octave = pitch >> 11
    if octave & 8: octave -= 16
    return 261.6256 * (1.0 + (pitch & 1023) / 1024.0) * 2.0 ** octave


def band(value: float, low: float, high: float) -> int:
    if high <= low: return BANDS // 2
    return min(BANDS - 1, max(0, round((math.log2(value) - math.log2(low)) *
                                       (BANDS - 1) / (math.log2(high) - math.log2(low)))))


def build(afx: Path) -> bytes:
    actions, duration, rate_num, rate_den, setups = decode(afx)
    metadata = afx_metadata.read(afx.read_bytes()) or {}
    keys = metadata.get("visual_note_keys")
    note_count = sum(action[0] in (NOTE, NOTE_PL) for _, action in actions)
    if keys is not None and not (isinstance(keys, list) and len(keys) == note_count and
                                 all(isinstance(key, int) and 0 <= key <= 127 for key in keys)):
        raise ValueError("invalid visual-note metadata")
    note_frequencies, note = [], 0
    for _, (opcode, _, setup, mask, values) in actions:
        if opcode not in (NOTE, NOTE_PL): continue
        state = setups[setup].copy()
        apply(state, mask, values.copy())
        note_frequencies.append(440.0 * 2.0 ** ((keys[note] - 69) / 12.0)
                                if keys is not None else frequency(state))
        note += 1
    low, high = min(note_frequencies, default=1.0), max(note_frequencies, default=1.0)
    if not rate_num or not rate_den: raise ValueError("AFX tick rate is zero")
    frames = max(1, math.ceil(duration * rate_den * RATE / rate_num))
    levels = [[0.0] * BANDS for _ in range(frames)]
    active: list[tuple[list[int], float | None, int] | None] = [None] * 64
    action = note = 0
    for frame in range(frames):
        tick = frame * rate_num // (RATE * rate_den)
        while action < len(actions) and actions[action][0] <= tick:
            opcode, channel, setup, mask, values = actions[action][1]
            if opcode in (NOTE, NOTE_PL):
                state = setups[setup].copy()
                apply(state, mask, values.copy())
                voice_frequency = 440.0 * 2.0 ** ((keys[note] - 69) / 12.0) if keys is not None else None
                active[channel] = (state, voice_frequency, actions[action][0])
                note += 1
            elif opcode in (PATCH, PATCH_LEVEL) and active[channel] is not None:
                apply(active[channel][0], mask, values.copy())
            elif opcode == KEYOFF: active[channel] = None
            action += 1
        for voice in active:
            if voice is not None:
                state, voice_frequency, started = voice
                attenuation = state[TOTAL_LEVEL] >> 8
                age = (tick - started) * rate_den / rate_num
                # ponytail: one visual half-life for every instrument; use sample/envelope
                # analysis if faithful metering is needed. Square for the sqrt below.
                decay = 2.0 ** (-2.0 * age / NOTE_HALF_LIFE_SECONDS)
                levels[frame][band(voice_frequency if voice_frequency is not None else frequency(state), low, high)] += 10.0 ** (-attenuation / 25.0) * decay
    ceiling = max((value for frame in levels for value in frame), default=1.0) or 1.0
    payload = bytes(round(255.0 * math.sqrt(value / ceiling)) for frame in levels for value in frame)
    return HEADER.pack(MAGIC, VERSION, BANDS, RATE, 0, frames) + payload


def write(afx: Path, output: Path) -> None:
    output.write_bytes(build(afx))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("afx", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    write(args.afx, args.output)
    return 0


if __name__ == "__main__": raise SystemExit(main())
