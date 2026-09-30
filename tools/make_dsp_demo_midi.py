#!/usr/bin/env python3
"""Generate four short, DSP-send sine hits for the ping-pong delay demo."""

import sys
from pathlib import Path

import mido


if len(sys.argv) != 2:
    raise SystemExit(f"usage: {sys.argv[0]} output.mid")

output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
midi = mido.MidiFile(ticks_per_beat=480)
track = mido.MidiTrack()
midi.tracks.append(track)
track.extend((
    mido.Message("program_change", channel=0, program=0, time=480),
    mido.Message("control_change", channel=0, control=91, value=127, time=0),
))
for note in (72, 76, 79, 84):
    track.append(mido.Message("note_on", channel=0, note=note, velocity=110, time=0))
    track.append(mido.Message("note_off", channel=0, note=note, velocity=0, time=180))
    track.append(mido.Message("control_change", channel=0, control=7, value=100, time=300))
track.append(mido.MetaMessage("end_of_track", time=0))
midi.save(output)
