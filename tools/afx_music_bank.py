#!/usr/bin/env python3
"""Pack self-contained N64-imported AFX music files into one shared AFB1 bank."""

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

import afx_compile


HEADER = struct.Struct("<4sHH7I")
SAMPLE = struct.Struct("<4I")
SOUND = struct.Struct("<HBBHHII")
SETUP = struct.Struct("<HHI18H")
AFC_HEADER = struct.Struct("<4sHBBHII")
AFX_HEADER = struct.Struct("<20I")
AFX_SAMPLE = struct.Struct("<4I")
AFX_RELOCATION = struct.Struct("<3I")
SETUP_BYTES = 36
PCM16, PCM8, ADPCM = 0, 1, 2
MUSIC_FLAGS = 2 | 8 | 16
METADATA_FLAG = 4


def align(value: int) -> int:
    return (value + 31) & -32


def _accepted_adpcm(path: Path) -> set[str]:
    audit = json.loads(path.read_text())
    thresholds = audit["thresholds"]
    return {entry["sha256"] for entry in audit["samples"]
            if entry["snr_db"] >= thresholds["snr_db"] and
            entry["attack_snr_db"] >= thresholds["attack_snr_db"]}


def _flow_id(path: Path) -> int:
    match = re.fullmatch(r"sequence_(\d+)\.afx", path.name)
    if not match or not 1 <= int(match.group(1)) <= 65535:
        raise ValueError(f"{path} is not a numbered sequence_*.afx file")
    return int(match.group(1))


def _read_flow(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < AFX_HEADER.size:
        raise ValueError(f"{path}: shorter than AFX header")
    (magic, abi, total, flags, image_at, image_size, stream_at, stream_size,
     setups_at, setup_count, samples_at, sample_count, relocations_at, relocation_count,
     checkpoints_at, checkpoints_size, channels, tick_num, tick_den, _) = AFX_HEADER.unpack_from(data)
    if magic != 0x32584641 or abi != 6 or total != len(data) or flags & ~(MUSIC_FLAGS | METADATA_FLAG) or not flags & 2:
        raise ValueError(f"{path}: not an ABI-6 AFX music flow")
    if channels > 64 or not tick_num or not tick_den or tick_num != 1000 or tick_den != 1:
        raise ValueError(f"{path}: unsupported channel or tick configuration")
    if image_at > len(data) or image_size > len(data) - image_at or stream_at > image_size or stream_size > image_size - stream_at:
        raise ValueError(f"{path}: image range is invalid")
    image = data[image_at:image_at + image_size]
    if not channels:
        return {"id": _flow_id(path), "silent": True}
    if setups_at != 0 or setup_count > image_size // SETUP_BYTES or stream_at < setup_count * SETUP_BYTES:
        raise ValueError(f"{path}: unsupported setup layout")
    lanes = image[setup_count * SETUP_BYTES:stream_at]
    if (flags & 16 and len(lanes) != channels) or (not flags & 16 and lanes):
        raise ValueError(f"{path}: invalid lane map")
    if samples_at + sample_count * AFX_SAMPLE.size > image_at or relocations_at + relocation_count * AFX_RELOCATION.size > image_at:
        raise ValueError(f"{path}: metadata table is invalid")
    samples = [AFX_SAMPLE.unpack_from(data, samples_at + i * AFX_SAMPLE.size) for i in range(sample_count)]
    for offset, size, frames, format_id in samples:
        if not size or not frames or format_id > ADPCM or offset > image_size or size > image_size - offset:
            raise ValueError(f"{path}: invalid sample")
    relocation = {}
    for i in range(relocation_count):
        pair, sample, byte_offset = AFX_RELOCATION.unpack_from(data, relocations_at + i * AFX_RELOCATION.size)
        if pair % SETUP_BYTES or pair // SETUP_BYTES >= setup_count or sample >= sample_count or byte_offset:
            raise ValueError(f"{path}: unsupported relocation")
        if pair in relocation:
            raise ValueError(f"{path}: duplicate relocation")
        relocation[pair // SETUP_BYTES] = sample
    if len(relocation) != setup_count:
        raise ValueError(f"{path}: every setup must name one sample")
    return {"id": _flow_id(path), "flags": flags & MUSIC_FLAGS, "channels": channels, "samples": samples, "image": image,
            "setup_count": setup_count, "setups": image[:setup_count * SETUP_BYTES],
            "relocation": relocation, "lanes": lanes, "stream": image[stream_at:stream_at + stream_size]}


def build_bank(paths: list[Path], accepted_adpcm: set[str]) -> tuple[bytes, dict]:
    flows = []
    for path in paths:
        flow = _read_flow(path)
        if not flow.get("silent"):
            flows.append(flow)
    if len({flow["id"] for flow in flows}) != len(flows):
        raise ValueError("duplicate sequence id")
    sample_by_key, encoded_by_source, samples, records, setups, streams = {}, {}, [], [], [], bytearray()
    for flow in sorted(flows, key=lambda item: item["id"]):
        first_setup = len(setups)
        local = {}
        for index in sorted(set(flow["relocation"].values())):
            offset, size, frames, format_id = flow["samples"][index]
            payload = flow["image"][offset:offset + size]
            source_key = hashlib.sha256(payload).digest(), frames, format_id
            encoded = encoded_by_source.get(source_key)
            if encoded is None:
                if format_id == PCM16:
                    digest = source_key[0].hex()
                    payload, format_id = ((afx_compile.pcm16_to_adpcm(payload), ADPCM)
                                         if digest in accepted_adpcm else
                                         (afx_compile.pcm16_to_pcm8(payload), PCM8))
                encoded = payload, format_id
                encoded_by_source[source_key] = encoded
            payload, format_id = encoded
            key = hashlib.sha256(payload).digest(), frames, format_id
            local[index] = sample_by_key.setdefault(key, len(samples))
            if local[index] == len(samples):
                samples.append((payload, frames, format_id))
        for index in range(flow["setup_count"]):
            fields = list(struct.unpack_from("<18H", flow["setups"], index * SETUP_BYTES))
            fields[0] &= ~0x1ff
            fields[1] = 0
            setups.append((local[flow["relocation"][index]], fields))
        streams += flow["lanes"]
        stream_at = len(streams)
        streams += flow["stream"]
        records.append((flow["id"], flow["channels"], flow["flags"], first_setup,
                        flow["setup_count"], stream_at, len(flow["stream"])))
    if len(setups) > 65535:
        raise ValueError("AFB1 setup table exceeds 65535 entries")
    sample_at = HEADER.size
    sound_at = sample_at + len(samples) * SAMPLE.size
    setup_at = sound_at + len(records) * SOUND.size
    stream_at = setup_at + len(setups) * SETUP.size
    data_at = align(stream_at + len(streams))
    offsets, cursor = [], data_at
    for payload, _, _ in samples:
        cursor = align(cursor)
        offsets.append(cursor)
        cursor += len(payload)
    image = bytearray(cursor)
    HEADER.pack_into(image, 0, b"AFB1", 1, len(records), len(samples), sample_at, sound_at,
                     setup_at, stream_at, data_at, len(image))
    for index, ((payload, frames, format_id), offset) in enumerate(zip(samples, offsets)):
        SAMPLE.pack_into(image, sample_at + index * SAMPLE.size, offset, len(payload), frames, format_id)
        image[offset:offset + len(payload)] = payload
    for index, record in enumerate(records):
        number, channels, flags, first, count, relative, size = record
        SOUND.pack_into(image, sound_at + index * SOUND.size, number, channels, flags, first, count,
                        stream_at + relative, size)
    for index, (sample, fields) in enumerate(setups):
        SETUP.pack_into(image, setup_at + index * SETUP.size, sample, 0, 0, *fields)
    image[stream_at:stream_at + len(streams)] = streams
    diagnostics = {
        "flows": len(records), "samples": len(samples), "setups": len(setups),
        "pcm8_bytes": sum(len(raw) for raw, _, format_id in samples if format_id == PCM8),
        "adpcm_bytes": sum(len(raw) for raw, _, format_id in samples if format_id == ADPCM),
        "sample_bytes": sum(len(raw) for raw, _, _ in samples), "stream_bytes": len(streams),
        "total_bytes": len(image),
    }
    return bytes(image), diagnostics


def build_split_bank(paths: list[Path], accepted_adpcm: set[str]) -> tuple[bytes, dict[int, bytes], dict]:
    """Build one shared sample bank and one compact AFC1 control file per flow."""
    flows = [flow for path in paths if not (flow := _read_flow(path)).get("silent")]
    if len({flow["id"] for flow in flows}) != len(flows):
        raise ValueError("duplicate sequence id")
    sample_by_key, encoded_by_source, samples, controls = {}, {}, [], {}
    for flow in sorted(flows, key=lambda item: item["id"]):
        local = {}
        for index in sorted(set(flow["relocation"].values())):
            offset, size, frames, format_id = flow["samples"][index]
            payload = flow["image"][offset:offset + size]
            source_key = hashlib.sha256(payload).digest(), frames, format_id
            encoded = encoded_by_source.get(source_key)
            if encoded is None:
                if format_id == PCM16:
                    digest = source_key[0].hex()
                    payload, format_id = ((afx_compile.pcm16_to_adpcm(payload), ADPCM)
                                         if digest in accepted_adpcm else
                                         (afx_compile.pcm16_to_pcm8(payload), PCM8))
                encoded = payload, format_id
                encoded_by_source[source_key] = encoded
            payload, format_id = encoded
            key = hashlib.sha256(payload).digest(), frames, format_id
            local[index] = sample_by_key.setdefault(key, len(samples))
            if local[index] == len(samples):
                samples.append((payload, frames, format_id))
        setups = bytearray()
        for index in range(flow["setup_count"]):
            fields = list(struct.unpack_from("<18H", flow["setups"], index * SETUP_BYTES))
            fields[0] &= ~0x1ff
            fields[1] = 0
            setups += SETUP.pack(local[flow["relocation"][index]], 0, 0, *fields)
        control_size = AFC_HEADER.size + len(setups) + len(flow["lanes"]) + len(flow["stream"])
        controls[flow["id"]] = (AFC_HEADER.pack(b"AFC1", 1, flow["channels"], flow["flags"],
                                                  flow["setup_count"], len(flow["stream"]), control_size) +
                                setups + flow["lanes"] + flow["stream"])
    sample_at = HEADER.size
    sound_at = sample_at + len(samples) * SAMPLE.size
    data_at = align(sound_at)
    offsets, cursor = [], data_at
    for payload, _, _ in samples:
        cursor = align(cursor)
        offsets.append(cursor)
        cursor += len(payload)
    image = bytearray(cursor)
    HEADER.pack_into(image, 0, b"AFB1", 1, 0, len(samples), sample_at, sound_at,
                     sound_at, sound_at, data_at, len(image))
    for index, ((payload, frames, format_id), offset) in enumerate(zip(samples, offsets)):
        SAMPLE.pack_into(image, sample_at + index * SAMPLE.size, offset, len(payload), frames, format_id)
        image[offset:offset + len(payload)] = payload
    diagnostics = {
        "flows": len(controls), "samples": len(samples),
        "pcm8_bytes": sum(len(raw) for raw, _, format_id in samples if format_id == PCM8),
        "adpcm_bytes": sum(len(raw) for raw, _, format_id in samples if format_id == ADPCM),
        "sample_bytes": sum(len(raw) for raw, _, _ in samples),
        "control_bytes": sum(map(len, controls.values())), "total_bytes": len(image),
    }
    return bytes(image), controls, diagnostics


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("audit", type=Path)
    parser.add_argument("--controls-dir", type=Path)
    parser.add_argument("flow", type=Path, nargs="+")
    args = parser.parse_args(argv)
    try:
        if args.controls_dir:
            image, controls, diagnostics = build_split_bank(args.flow, _accepted_adpcm(args.audit))
            args.controls_dir.mkdir(parents=True, exist_ok=True)
            for ident, control in controls.items():
                (args.controls_dir / f"sequence_{ident}.afc").write_bytes(control)
        else:
            image, diagnostics = build_bank(args.flow, _accepted_adpcm(args.audit))
        args.output.write_bytes(image)
    except (OSError, ValueError, struct.error, json.JSONDecodeError) as error:
        print(f"afx-music-bank: {error}", file=sys.stderr)
        return 2
    print(f"wrote {args.output}: {diagnostics}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
