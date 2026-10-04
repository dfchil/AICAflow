#!/usr/bin/env python3
"""Check the bounded hardware fixture keeps panic/reset semantics in its AFX input."""

import afx_midi
import subprocess
import tempfile
from pathlib import Path



with tempfile.TemporaryDirectory() as directory:
    midi = Path(directory) / "fixture.mid"
    subprocess.run(["python3", str(Path(afx_midi.__file__).with_name("make_fixture_midi.py")), str(midi)], check=True)
    timeline = afx_midi.parse(midi)
    assert [(note["key"], note["start_tick"], note["end_tick"])
            for note in timeline["notes"]] == [(69, 0, 4320), (81, 4800, 5280), (84, 7680, 8160)]
    assert not timeline["warnings"]

print("hardware fixture MIDI panic/reset checks passed")
