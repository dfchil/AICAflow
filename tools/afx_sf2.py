#!/usr/bin/env python3
"""Report exact SoundFont PCM-region cost for an explicit MIDI mapping.

This is a selection/budget stage, not an automatic converter: choosing left
versus stereo is an author decision and an over-budget result never changes it.
"""

import argparse
import json
import sys
from pathlib import Path

from sf2utils.sf2parse import Sf2File


import afx_midi


def parse_triplet(value: str) -> tuple[int, int, int]:
    parts = value.split(":")
    if len(parts) != 3 or any(not part.isdigit() for part in parts):
        raise ValueError("source program must be bank-msb:bank-lsb:program")
    return tuple(map(int, parts))


def parse_pair(value: str) -> tuple[int, int]:
    parts = value.split(":")
    if len(parts) != 2 or any(not part.isdigit() for part in parts):
        raise ValueError("SoundFont preset must be bank:program")
    return tuple(map(int, parts))


def in_range(value: int, allowed) -> bool:
    return allowed is None or allowed[0] <= value <= allowed[1]


def source_controls(preset, preset_bag, instrument_bag) -> dict:
    """Combine the selected global/local SF2 controls without pretending to render them."""
    levels = ([bag for bag in preset.bags if bag.instrument is None] + [preset_bag],
              [bag for bag in preset_bag.instrument.bags if bag.sample is None] + [instrument_bag])
    generators = {
        "attack_timecents": 34, "decay_timecents": 36, "sustain_centibels": 37,
        "release_timecents": 38, "attenuation_centibels": 48,
        "pan_centibels": 17, "reverb_send_per_mille": 16,
        "chorus_send_per_mille": 15,
        "filter_cutoff_cents": 8, "filter_q_centibels": 9,
        "mod_env_filter_cents": 11,
        "mod_env_attack_timecents": 26, "mod_env_decay_timecents": 28,
        "mod_env_sustain_per_mille": 29, "mod_env_release_timecents": 30,
        "mod_env_key_decay_timecents": 32,
        "coarse_tune": 51, "fine_tune": 52,
    }
    result = {}
    for name, oper in generators.items():
        # Local generators replace globals at each level; preset and instrument
        # values then add. Summing all four bags doubles overridden envelopes.
        values = [next((getattr(bag, "gens", {})[oper].short for bag in reversed(level)
                        if oper in getattr(bag, "gens", {})), None) for level in levels]
        # Preset values offset the instrument default when that generator is
        # absent from the instrument (SF2 §9.4), not an implicit zero.
        defaults = {8: 13500, 26: -12000, 28: -12000, 30: -12000,
                    34: -12000, 36: -12000, 38: -12000}
        if values[0] is not None and values[1] is None:
            values[1] = defaults.get(oper, 0)
        values = [value for value in values if value is not None]
        result[name] = sum(values) if values else None
    return result


def modulator_amount(preset, preset_bag, instrument_bag, identity=(1282, 48, 0, 0), default=960) -> int:
    """Resolve one modulator: instrument overrides default, preset adds to it."""
    amount = 0
    levels = (([b for b in preset.bags if b.instrument is None] + [preset_bag], 0),
              ([b for b in preset_bag.instrument.bags if b.sample is None] + [instrument_bag], default))
    for bags, value in levels:
        for bag in bags:
            for mod in getattr(bag, "mods", []):
                if (mod.src_oper, mod.dest_oper, mod.amount_src_oper, mod.trans_oper) == identity:
                    value = mod.amount if mod.amount < 32768 else mod.amount - 65536
        amount += value
    return amount


def select_note_uses(note: dict, preset, channel: str) -> list[dict]:
    """Return selected SoundFont zones, collapsing mono-encoded stereo mirrors."""
    uses = []
    for preset_bag in preset.key_bags(note["key"]):
        if not in_range(note["velocity"], preset_bag.velocity_range):
            continue
        for bag in preset_bag.instrument.bags:
            sample = bag.sample
            if sample is None or not in_range(note["key"], bag.key_range) or \
               not in_range(note["velocity"], bag.velocity_range):
                continue
            selected = sample.is_mono or channel == "stereo" or \
                       (channel == "left" and sample.is_left) or \
                       (channel == "right" and not sample.is_left)
            if selected:
                instrument_bags = [b for b in preset_bag.instrument.bags if b.sample is None] + [bag]
                modes = next((b.gens[54].short for b in reversed(instrument_bags)
                              if 54 in getattr(b, "gens", {})), None)
                controls = source_controls(preset, preset_bag, bag)
                coarse, fine = controls.pop("coarse_tune"), controls.pop("fine_tune")
                root = next((b.gens[58].short for b in reversed(instrument_bags)
                             if 58 in getattr(b, "gens", {})), bag.base_note)
                uses.append({"sample": sample,
                             "root_key": sample.original_pitch if root is None or root < 0 else root,
                             "tuning_cents": sample.pitch_correction +
                                             (coarse if coarse is not None else bag.tuning or 0) * 100 +
                                             (fine if fine is not None else bag.fine_tuning or 0),
                             "loop": bool(bag.sample_loop) if modes is None else bool(modes & 1),
                             "velocity_attenuation_amount": modulator_amount(preset, preset_bag, bag),
                             "velocity_filter_amount": modulator_amount(preset, preset_bag, bag, (258, 8, 0, 0), 0),
                             "source_controls": controls})
    # Some banks encode a stereo pair as two independent mono zones with
    # equal controls except pan.  ``left`` / ``right`` must not accidentally
    # turn that representation into two music voices.  Preserve unrelated
    # layers: only a complete mirror pair is collapsed.
    if channel == "stereo":
        return uses
    grouped = {}
    for use in uses:
        controls = tuple(sorted((key, value) for key, value in use["source_controls"].items()
                                if key != "pan_centibels"))
        key = (use["root_key"], use["tuning_cents"], use["loop"], controls,
               use["velocity_attenuation_amount"], use["velocity_filter_amount"])
        grouped.setdefault(key, []).append(use)
    selected = []
    for group in grouped.values():
        pans = [use["source_controls"]["pan_centibels"] for use in group]
        if len(group) == 2 and all(isinstance(pan, int) for pan in pans) and min(pans) < 0 < max(pans):
            selected.append(min(group, key=lambda use: use["source_controls"]["pan_centibels"])
                           if channel == "left" else
                           max(group, key=lambda use: use["source_controls"]["pan_centibels"]))
        else:
            selected.extend(group)
    return selected


def select_region_uses(notes: list[dict], preset, channel: str) -> dict:
    """Select actual SoundFont zones, retaining their authoritative root key.

    ``preset.key_samples()`` loses the instrument-zone override root key.  That
    is harmless for byte accounting but would make an AICA compile detune banks
    whose sample headers use a placeholder root.  Do not collapse that detail.
    """
    regions = {}
    for note in notes:
        for use in select_note_uses(note, preset, channel):
            sample = use["sample"]
            identity = (sample.start, sample.end, sample.sample_type)
            region = regions.setdefault(identity, {"sample": sample, "roots": set(), "tunes": set(),
                                                   "loop_modes": set(), "uses": 0})
            region["roots"].add(use["root_key"])
            region["tunes"].add(use["tuning_cents"])
            region["loop_modes"].add(use["loop"])
            region["uses"] += 1
    return regions


def select_regions(notes: list[dict], preset, channel: str) -> list:
    """Compatibility helper for selection-policy callers."""
    return [region["sample"] for region in select_region_uses(notes, preset, channel).values()]


def report(midi: Path, soundfont: Path, source_program: tuple[int, int, int],
           sf2_preset: tuple[int, int], channel: str, asset_budget: int | None) -> dict:
    timeline = afx_midi.parse(midi)
    notes = [note for note in timeline["notes"] if
             (note["bank_msb"], note["bank_lsb"], note["program"]) == source_program]
    if not notes:
        raise ValueError(f"source program {source_program} has no notes")
    with soundfont.open("rb") as input_file:
        font = Sf2File(input_file)
        preset = next((candidate for candidate in font.presets
                       if candidate.bank == sf2_preset[0] and candidate.preset == sf2_preset[1]), None)
        if not preset:
            available = ", ".join(f"{candidate.bank}:{candidate.preset}" for candidate in font.presets)
            raise ValueError(f"SoundFont preset {sf2_preset} missing; available: {available or 'none'}")
        regions = select_region_uses(notes, preset, channel)
        if not regions:
            raise ValueError(f"no {channel} PCM regions selected")
        # sf2utils resolves raw_sample_data lazily through input_file; materialize
        # report metadata before leaving this scope.
        details = [{"name": region["sample"].name,
                    "bytes": len(region["sample"].raw_sample_data),
                    "frames": len(region["sample"].raw_sample_data) // 2,
                    "loop_start": region["sample"].start_loop,
                    "loop_end": region["sample"].end_loop,
                    "root_keys": sorted(region["roots"]),
                    "tuning_cents": sorted(region["tunes"]),
                    "loop_modes": sorted(region["loop_modes"]),
                    "uses": region["uses"]} for region in regions.values()]
    bytes_needed = sum(region["bytes"] for region in details)
    result = {
        "source_program": ":".join(map(str, source_program)),
        "soundfont_preset": ":".join(map(str, sf2_preset)),
        "channel_policy": channel,
        "note_count": len(notes),
        "sample_regions": len(regions),
        "pcm16_bytes": bytes_needed,
        "regions": details,
    }
    if asset_budget is not None:
        result["asset_budget"] = asset_budget
        result["fits_asset_budget"] = bytes_needed <= asset_budget
        result["asset_margin"] = asset_budget - bytes_needed
        if bytes_needed > asset_budget:
            result["alternatives"] = [
                "author a smaller key/velocity region selection",
                "author a mono channel policy if stereo was selected",
                "author a loop/interpolation or approved compression strategy",
            ]
    return result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("midi", type=Path)
    parser.add_argument("soundfont", type=Path)
    parser.add_argument("--source-program", required=True)
    parser.add_argument("--sf2-preset", required=True)
    parser.add_argument("--channel", choices=("left", "right", "stereo"), required=True)
    parser.add_argument("--asset-budget", type=int)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = report(args.midi, args.soundfont, parse_triplet(args.source_program),
                        parse_pair(args.sf2_preset), args.channel, args.asset_budget)
    except (OSError, ValueError) as error:
        print(f"afx-sf2: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        for key in ("note_count", "sample_regions", "pcm16_bytes", "fits_asset_budget", "asset_margin"):
            if key in result:
                print(f"{key}: {result[key]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
