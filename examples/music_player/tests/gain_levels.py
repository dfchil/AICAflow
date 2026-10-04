"""Compare old/new base assets: about +4 dB, unchanged timing and samples.

Usage: python3 tests/gain_levels.py OLD_ASSET_DIR NEW_ASSET_DIR
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] /
                       "dependencies/AICAforge/research"))
from afx_visualize import decode, apply, NOTE, NOTE_PL, KEYOFF


def states(path):
    actions, duration, numerator, denominator, setups = decode(path)
    active = {}
    result = []
    for tick, (opcode, channel, setup, mask, values) in actions:
        if opcode in (NOTE, NOTE_PL):
            active[channel] = setups[setup].copy()
            kind = "note"
        elif opcode == KEYOFF:
            result.append((tick, channel, "off", None))
            continue
        else:
            kind = "patch"
        apply(active[channel], mask, values.copy())
        result.append((tick, channel, kind, active[channel].copy()))
    return (duration, numerator, denominator), result


old_dir, new_dir = map(Path, sys.argv[1:])
for name in ("chopin-op27-no1", "bach-bwv1007-prelude", "grieg-mountain-king"):
    assert (old_dir / f"{name}.afb").read_bytes() == (new_dir / f"{name}.afb").read_bytes(), name
    old_time, old = states(old_dir / f"{name}.afx")
    new_time, new = states(new_dir / f"{name}.afx")
    assert old_time == new_time and len(old) == len(new), name
    for before, after in zip(old, new):
        assert before[:3] == after[:3], (name, before[:3], after[:3])
        if before[3] is None:
            continue
        expected = before[3].copy()
        level = expected[10] >> 8
        assert level >= 11, (name, "insufficient per-voice headroom", level)
        # Gain bias -400 yields 10 or 11 TL steps after register quantization.
        delta = level - (after[3][10] >> 8)
        assert delta in (10, 11), (name, before[:3], delta)
        expected[10] -= delta << 8
        assert after[3] == expected, (name, before[:3], expected, after[3])
    print(f"{name}: samples/timing/registers preserved; levels raised by 10–11 TL steps")
