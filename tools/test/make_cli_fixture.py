"""Synthetic CLI inputs: no runtime checkout or copyrighted sample required."""
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
research = Path(__file__).resolve().parents[1] / "research"
subprocess.run([sys.executable, str(research / "make_fixture_midi.py"),
                str(root / "fixture.mid")], check=True)
(root / "tone.pcm").write_bytes(bytes(range(128)))
(root / "tone.adpcm").write_bytes(bytes(range(256)))
(root / "fixture.zones").write_text(
    f"0 70 69 -1 -1 pcm8 {root / 'tone.pcm'}\n"
    f"71 127 81 0 127 adpcm {root / 'tone.adpcm'}\n")
