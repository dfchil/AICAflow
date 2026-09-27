#!/usr/bin/env python3
"""Measure loss and space savings from re-encoding PCM16 AFX samples as ADPCM."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from collections import defaultdict
from pathlib import Path

import afx_adpcm
import afx_compile

HEADER = struct.Struct("<20I")
SAMPLE = struct.Struct("<4I")


def pcm16_samples(path: Path):
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError(f"{path}: truncated AFX header")
    _, _, total, _, image_at, image_bytes, _, _, _, _, samples_at, sample_count, *_ = HEADER.unpack_from(data)
    if total != len(data) or image_at + image_bytes > len(data):
        raise ValueError(f"{path}: invalid AFX image bounds")
    if samples_at + sample_count * SAMPLE.size > image_at:
        raise ValueError(f"{path}: invalid AFX sample table")
    for index in range(sample_count):
        offset, size, frames, sample_format = SAMPLE.unpack_from(data, samples_at + index * SAMPLE.size)
        if sample_format != afx_compile.PCM16:
            continue
        if size != frames * 2 or offset + size > image_bytes:
            raise ValueError(f"{path}: invalid PCM16 sample {index}")
        yield data[image_at + offset:image_at + offset + size], index


def snr(source: bytes, decoded: bytes, frames: int) -> tuple[float, int]:
    original = struct.unpack(f"<{frames}h", source)
    restored = struct.unpack(f"<{frames}h", decoded)
    signal = sum(value * value for value in original)
    error = sum((left - right) ** 2 for left, right in zip(original, restored))
    peak_error = max((abs(left - right) for left, right in zip(original, restored)), default=0)
    return (99.0 if not error else 10.0 * math.log10(signal / error) if signal else 0.0), peak_error


def audit(paths: list[Path], min_snr: float, min_attack_snr: float) -> dict:
    unique: dict[str, tuple[bytes, list[str]]] = {}
    for path in paths:
        for raw, index in pcm16_samples(path):
            digest = hashlib.sha256(raw).hexdigest()
            unique.setdefault(digest, (raw, []))[1].append(f"{path.stem}:{index}")
    samples = []
    for digest, (raw, uses) in unique.items():
        frames = len(raw) // 2
        encoded = afx_adpcm.pcm16_to_adpcm(raw)
        decoded = afx_compile.aica_adpcm_decode(encoded, frames)
        quality, peak_error = snr(raw, decoded, frames)
        attack_frames = min(frames, 1024)
        attack_quality, _ = snr(raw[:attack_frames * 2], decoded[:attack_frames * 2], attack_frames)
        samples.append({"sha256": digest, "frames": frames, "pcm16_bytes": len(raw),
                        "adpcm_bytes": len(encoded), "snr_db": round(quality, 3),
                        "attack_snr_db": round(attack_quality, 3), "max_error": peak_error,
                        "uses": uses})
    samples.sort(key=lambda sample: sample["sha256"])
    accepted = [sample for sample in samples if sample["snr_db"] >= min_snr and
                sample["attack_snr_db"] >= min_attack_snr]
    return {"format": "aica-adpcm-ya2beam-v1", "sample_count": len(samples),
            "pcm16_bytes": sum(sample["pcm16_bytes"] for sample in samples),
            "adpcm_bytes": sum(sample["adpcm_bytes"] for sample in samples),
            "accepted_count": len(accepted),
            "accepted_pcm16_bytes": sum(sample["pcm16_bytes"] for sample in accepted),
            "accepted_adpcm_bytes": sum(sample["adpcm_bytes"] for sample in accepted),
            "thresholds": {"snr_db": min_snr, "attack_snr_db": min_attack_snr}, "samples": samples}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="directory containing sequence_*.afx files")
    parser.add_argument("output", type=Path, help="JSON report path")
    parser.add_argument("--snr", type=float, default=30.0, help="minimum full-sample SNR in dB (default: 30)")
    parser.add_argument("--attack-snr", type=float, default=24.0, help="minimum first-1024-frame SNR in dB (default: 24)")
    args = parser.parse_args()
    paths = sorted(args.directory.glob("sequence_*.afx"))
    if not paths:
        parser.error(f"no sequence_*.afx files in {args.directory}")
    report = audit(paths, args.snr, args.attack_snr)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    saved = report["accepted_pcm16_bytes"] - report["accepted_adpcm_bytes"]
    print(f"{report['sample_count']} unique PCM16 samples; {report['accepted_count']} pass; {saved} bytes saved")


if __name__ == "__main__":
    main()
