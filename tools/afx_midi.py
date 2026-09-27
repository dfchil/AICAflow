#!/usr/bin/env python3
"""Resolve Standard MIDI into a source-faithful performance timeline.

This is deliberately before AICA lowering: times remain MIDI ticks and a note
keeps its own identity through sustain/repeated-note handling.  Instrument
mapping, release modelling and quantization belong to later compiler stages.
"""

import argparse
import heapq
from io import BytesIO
from importlib.metadata import version
import json
import sys
from pathlib import Path

try:
    import mido
except ModuleNotFoundError as error:  # Existing toolchain dependency, never silent.
    raise SystemExit("afx-midi: mido is required; install the recorded tool dependency") from error


def parse(path: Path) -> dict:
    return parse_midi(mido.MidiFile(path), str(path))


def parse_bytes(data: bytes, source: str = "<embedded>") -> dict:
    return parse_midi(mido.MidiFile(file=BytesIO(data)), source)


def parse_midi(midi: mido.MidiFile, source: str) -> dict:
    if midi.type not in (0, 1):
        raise ValueError(f"MIDI type {midi.type} is not supported")
    events = []
    final_tick = 0
    for track_index, track in enumerate(midi.tracks):
        tick = 0
        for order, message in enumerate(track):
            tick += message.time
            events.append((tick, track_index, order, message))
            final_tick = max(final_tick, tick)
    events.sort(key=lambda event: event[:3])

    programs = [0] * 16
    bank_msb = [0] * 16
    bank_lsb = [0] * 16
    volume = [127] * 16
    expression = [127] * 16
    pan = [64] * 16
    reverb = [0] * 16
    chorus = [0] * 16
    modulation = [0] * 16
    resonance = [64] * 16
    brightness = [64] * 16
    pitch_bend = [0] * 16
    bend_range_cents = [200] * 16
    rpn_msb = [127] * 16
    rpn_lsb = [127] * 16
    nrpn_msb = [127] * 16
    nrpn_lsb = [127] * 16
    parameter_mode = [None] * 16
    data_entry_msb = [0] * 16
    data_entry_lsb = [0] * 16
    sustain = [False] * 16
    active: dict[tuple[int, int], list[dict]] = {}
    pending: dict[int, list[dict]] = {channel: [] for channel in range(16)}
    notes, warnings, tempos, automation = [], [], [{"tick": 0, "us_per_beat": 500000}], []

    def finish(note: dict, tick: int, track: int, order: int) -> None:
        note["end_tick"] = tick
        note["end_track"], note["end_order"] = track, order
        notes.append(note)

    def finish_channel(channel: int, tick: int, track: int, order: int, respect_sustain: bool) -> None:
        for key in [key for key in active if key[0] == channel]:
            held = active.pop(key)
            if respect_sustain and sustain[channel]:
                pending[channel].extend(held)
            else:
                for note in held:
                    finish(note, tick, track, order)
        if not respect_sustain:
            for note in pending[channel]:
                finish(note, tick, track, order)
            pending[channel].clear()

    for tick, track_index, order, message in events:
        if message.is_meta:
            if message.type == "set_tempo":
                tempos.append({"tick": tick, "us_per_beat": message.tempo,
                               "track": track_index, "order": order})
            continue
        if message.type == "sysex":
            warnings.append({"kind": "unsupported_sysex", "tick": tick, "track": track_index,
                             "order": order, "bytes": len(message.data),
                             "requested": "System Exclusive device-specific performance change",
                             "fallback": "no AICA command emitted",
                             "missing_capability": "SysEx interpretation and AICA lowering"})
            continue
        if message.type == "program_change":
            programs[message.channel] = message.program
        elif message.type == "control_change":
            channel = message.channel
            if message.control == 0:
                bank_msb[channel] = message.value
            elif message.control == 32:
                bank_lsb[channel] = message.value
            elif message.control == 7:
                volume[channel] = message.value
            elif message.control == 10:
                pan[channel] = message.value
            elif message.control == 11:
                expression[channel] = message.value
            elif message.control == 1:
                modulation[channel] = message.value
            elif message.control == 71:
                resonance[channel] = message.value
            elif message.control == 74:
                brightness[channel] = message.value
            elif message.control == 91:
                reverb[channel] = message.value
            elif message.control == 93:
                chorus[channel] = message.value
            elif message.control == 101:
                rpn_msb[channel] = message.value
                parameter_mode[channel] = "rpn"
            elif message.control == 100:
                rpn_lsb[channel] = message.value
                parameter_mode[channel] = "rpn"
            elif message.control == 99:
                nrpn_msb[channel] = message.value
                parameter_mode[channel] = "nrpn"
            elif message.control == 98:
                nrpn_lsb[channel] = message.value
                parameter_mode[channel] = "nrpn"
            elif message.control in (6, 38):
                selected = (rpn_msb[channel], rpn_lsb[channel]) if parameter_mode[channel] == "rpn" else \
                           (nrpn_msb[channel], nrpn_lsb[channel]) if parameter_mode[channel] == "nrpn" else None
                if message.control == 6:
                    data_entry_msb[channel] = message.value
                else:
                    data_entry_lsb[channel] = message.value
                if parameter_mode[channel] == "rpn" and selected == (0, 0):
                    if message.control == 6:
                        bend_range_cents[channel] = message.value * 100 + bend_range_cents[channel] % 100
                    else:
                        bend_range_cents[channel] = (bend_range_cents[channel] // 100) * 100 + message.value
                elif parameter_mode[channel] == "nrpn" and selected == (1, 32):
                    # Round 14-bit Data Entry to the seven-bit control domain
                    # before the later four-bit AICA LPF lowering.
                    value14 = (data_entry_msb[channel] << 7) | data_entry_lsb[channel]
                    brightness[channel] = min(127, (value14 + 64) >> 7)
                    automation.append({"tick": tick, "track": track_index, "order": order,
                                       "channel": channel, "kind": "filter_cutoff",
                                       "nrpn": [1, 32], "value": brightness[channel], "value14": value14,
                                       "channel_volume": volume[channel],
                                       "channel_expression": expression[channel],
                                       "channel_pan": pan[channel], "channel_reverb": reverb[channel], "channel_chorus": chorus[channel],
                                       "channel_modulation": modulation[channel],
                                       "channel_resonance": resonance[channel],
                                       "channel_brightness": brightness[channel],
                                       "pitch_bend": pitch_bend[channel],
                                       "pitch_bend_range_cents": bend_range_cents[channel]})
                else:
                    warnings.append({"kind": "unsupported_parameter_data", "tick": tick,
                                     "track": track_index, "order": order, "channel": channel,
                                     "control": message.control, "value": message.value,
                                     "parameter_mode": parameter_mode[channel], "parameter": selected,
                                     "requested": "RPN/NRPN Data Entry performance change",
                                     "fallback": "no AICA command emitted",
                                     "missing_capability": "parameter-specific AICA lowering"})
            elif message.control == 64:
                was_held, sustain[channel] = sustain[channel], message.value >= 64
                if was_held and not sustain[channel]:
                    for note in pending[channel]:
                        finish(note, tick, track_index, order)
                    pending[channel].clear()
            elif message.control == 120:
                finish_channel(channel, tick, track_index, order, False)
            elif message.control == 123:
                finish_channel(channel, tick, track_index, order, True)
            elif message.control == 121:
                # GM RP-015 keeps bank, program, volume, pan, effects and sound
                # controllers. Pedal-held note-offs end as sustain drops.
                expression[channel], modulation[channel], pitch_bend[channel] = 127, 0, 0
                rpn_msb[channel], rpn_lsb[channel] = 127, 127
                nrpn_msb[channel], nrpn_lsb[channel], parameter_mode[channel] = 127, 127, None
                data_entry_msb[channel], data_entry_lsb[channel] = 0, 0
                was_held, sustain[channel] = sustain[channel], False
                if was_held:
                    for note in pending[channel]:
                        finish(note, tick, track_index, order)
                    pending[channel].clear()
            if message.control in (1, 7, 10, 11, 71, 74, 91, 93, 121):
                automation.append({"tick": tick, "track": track_index, "order": order,
                                   "channel": channel, "control": message.control,
                                   "value": message.value, "channel_volume": volume[channel],
                                   "channel_expression": expression[channel],
                                   "channel_pan": pan[channel], "channel_reverb": reverb[channel], "channel_chorus": chorus[channel],
                                   "channel_modulation": modulation[channel],
                                   "channel_resonance": resonance[channel],
                                   "channel_brightness": brightness[channel],
                                   "pitch_bend": pitch_bend[channel],
                                   "pitch_bend_range_cents": bend_range_cents[channel]})
            if message.control in (6, 38) and parameter_mode[channel] == "rpn" and \
                    (rpn_msb[channel], rpn_lsb[channel]) == (0, 0):
                automation.append({"tick": tick, "track": track_index, "order": order,
                                   "channel": channel, "kind": "pitch", "pitch_bend": pitch_bend[channel],
                                   "pitch_bend_range_cents": bend_range_cents[channel]})
            if message.control not in (0, 1, 32, 6, 7, 10, 11, 38, 64, 71, 74, 91, 93, 98, 99, 100, 101, 120, 121, 123):
                warnings.append({"kind": "unsupported_controller", "tick": tick, "track": track_index,
                                 "order": order, "channel": channel, "control": message.control,
                                 "value": message.value,
                                 "requested": "MIDI chorus send" if message.control == 93 else "MIDI controller change",
                                 "fallback": "no AICA command emitted",
                                 "missing_capability": "chorus-send lowering" if message.control == 93 else
                                                       "controller-specific AICA lowering"})
        elif message.type == "pitchwheel":
            channel = message.channel
            pitch_bend[channel] = message.pitch
            automation.append({"tick": tick, "track": track_index, "order": order,
                               "channel": channel, "kind": "pitch", "pitch_bend": message.pitch,
                               "pitch_bend_range_cents": bend_range_cents[channel]})
        elif message.type == "note_on" and message.velocity:
            channel, key = message.channel, message.note
            note = {"id": len(notes) + sum(len(stack) for stack in active.values()) +
                          sum(len(stack) for stack in pending.values()),
                    "start_tick": tick, "end_tick": None, "track": track_index,
                    "order": order, "channel": channel, "key": key,
                    "velocity": message.velocity, "bank_msb": bank_msb[channel],
                    "bank_lsb": bank_lsb[channel], "program": programs[channel],
                    "channel_volume": volume[channel], "channel_expression": expression[channel],
                    "channel_pan": pan[channel], "pitch_bend": pitch_bend[channel],
                    "pitch_bend_range_cents": bend_range_cents[channel], "channel_reverb": reverb[channel],
                    "channel_chorus": chorus[channel]}
            note["channel_modulation"] = modulation[channel]
            note["channel_resonance"] = resonance[channel]
            note["channel_brightness"] = brightness[channel]
            active.setdefault((channel, key), []).append(note)
        elif message.type == "note_off" or (message.type == "note_on" and not message.velocity):
            channel, key = message.channel, message.note
            stack = active.get((channel, key), [])
            if not stack:
                warnings.append({"kind": "orphan_note_off", "tick": tick,
                                 "track": track_index, "channel": channel, "key": key})
                continue
            # MIDI does not carry a note instance ID. FIFO preserves the oldest
            # still-held onset instead of collapsing repeated notes into one slot.
            note = stack.pop(0)
            if sustain[channel]:
                pending[channel].append(note)
            else:
                finish(note, tick, track_index, order)
        elif message.type in ("aftertouch", "polytouch"):
            warnings.append({"kind": "unsupported_pressure", "tick": tick, "track": track_index,
                             "order": order, "channel": message.channel, "message": message.type,
                             "requested": "channel pressure" if message.type == "aftertouch" else "polyphonic key pressure",
                             "fallback": "no AICA command emitted",
                             "missing_capability": "pressure-to-AICA modulation lowering"})

    for (channel, key), stack in active.items():
        for note in stack:
            finish(note, final_tick, len(midi.tracks), 0)
            warnings.append({"kind": "unclosed_note", "tick": final_tick,
                             "channel": channel, "key": key, "id": note["id"]})
    for channel, held in pending.items():
        for note in held:
            finish(note, final_tick, len(midi.tracks), 0)
            warnings.append({"kind": "unreleased_sustain", "tick": final_tick,
                             "channel": channel, "id": note["id"]})
    notes.sort(key=lambda note: (note["start_tick"], note["track"], note["order"], note["id"]))
    timeline = {"source": source, "parser": {"mido": version("mido")},
            "format": midi.type, "tracks": len(midi.tracks),
            "ticks_per_beat": midi.ticks_per_beat, "end_tick": final_tick,
            "tempos": tempos, "notes": notes, "automation": automation, "warnings": warnings}
    timeline["allocation"] = allocate_local_channels(notes)
    return timeline


def allocate_local_channels(notes: list[dict]) -> dict:
    """Optimal deterministic colouring for fixed half-open voice intervals.

    At one tick, a completed voice releases before a new voice starts, so a
    boundary-touching interval may reuse its local channel.  This is valid only
    after the later lowering step emits its same-tick KEYOFF before NOTE.
    """
    active: list[tuple[int, int]] = []  # (end tick, local channel)
    free: list[int] = []
    next_channel = peak = 0
    for note in notes:
        while active and active[0][0] <= note["start_tick"]:
            _, channel = heapq.heappop(active)
            heapq.heappush(free, channel)
        channel = heapq.heappop(free) if free else next_channel
        if channel == next_channel:
            next_channel += 1
        note["local_channel"] = channel
        heapq.heappush(active, (note["end_tick"], channel))
        peak = max(peak, len(active))
    return {"local_channels": next_channel, "peak_overlap": peak}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--json", action="store_true", help="emit the complete timeline")
    args = parser.parse_args(argv)
    try:
        timeline = parse(args.source)
    except (OSError, ValueError, EOFError) as error:
        print(f"afx-midi: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(timeline, sort_keys=True))
    else:
        print(f"tracks: {timeline['tracks']}")
        print(f"ticks per beat: {timeline['ticks_per_beat']}")
        print(f"notes: {len(timeline['notes'])}")
        print(f"local channels: {timeline['allocation']['local_channels']}")
        print(f"end tick: {timeline['end_tick']}")
        print(f"warnings: {len(timeline['warnings'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
