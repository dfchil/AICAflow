#!/usr/bin/env python3
"""Generate the audio and four control voices for the DSP effects player."""

import sys
from pathlib import Path

import mido


if len(sys.argv) != 2:
    raise SystemExit(f"usage: {sys.argv[0]} output.mid")

output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
midi = mido.MidiFile(ticks_per_beat=480)

# MIXS0 carries the audible phrase. MIXS1..4 are deliberately inaudible direct
# outputs, but provide useful moving control signals to modulation presets.
notes = ((72, 76, 79, 84), (24,), (31,), (36,), (43,))
for channel, pitches in enumerate(notes):
    track = mido.MidiTrack()
    midi.tracks.append(track)
    track.append(mido.Message("program_change", channel=channel, program=channel, time=0))
    track.append(mido.Message("control_change", channel=channel, control=91, value=127, time=0))
    if channel == 0:
        for note in pitches:
            track.append(mido.Message("note_on", channel=channel, note=note, velocity=110, time=240))
            track.append(mido.Message("note_off", channel=channel, note=note, velocity=0, time=180))
        track.append(mido.MetaMessage("end_of_track", time=1920))
    else:
        track.append(mido.Message("note_on", channel=channel, note=pitches[0], velocity=100, time=240))
        track.append(mido.Message("note_off", channel=channel, note=pitches[0], velocity=0, time=3000))
        track.append(mido.MetaMessage("end_of_track", time=0))
midi.save(output)
