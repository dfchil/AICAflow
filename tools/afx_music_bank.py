#!/usr/bin/env python3
"""Merge final AFB+AFX pairs into one shared AFB and rewritten AFX flows."""

import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path


AFB_MAGIC = 0x00424641
AFX_MAGIC = 0x32584641
AFB_VERSION = 1
AFX_VERSION = 7
BANK_HEADER = struct.Struct("<8I")
AFX_HEADER = struct.Struct("<20I")
AFX_RELOCATION = struct.Struct("<3I")
AFC_MAGIC = 0x00434641
AFC_HEADER = struct.Struct("<8I")
SETUP_BYTES = 36
CHECKPOINT_MAGIC = 0x31504B43


def align(value: int) -> int:
    return (value + 31) & -32


def _flow_id(path: Path) -> int:
    match = re.fullmatch(r"sequence_(\d+)\.afx", path.name)
    if not match or not 1 <= int(match.group(1)) <= 65535:
        raise ValueError(f"{path}: expected sequence_<1..65535>.afx")
    return int(match.group(1))


def _read_bank(path: Path) -> tuple[bytes, int, int]:
    data = path.read_bytes()
    if len(data) < BANK_HEADER.size:
        raise ValueError(f"{path}: truncated AFB header")
    magic, version, low, high, data_at, payload_bytes, total, reserved = BANK_HEADER.unpack_from(data)
    if (magic, version, total, reserved) != (AFB_MAGIC, AFB_VERSION, len(data), 0):
        raise ValueError(f"{path}: invalid AFB header")
    if data_at != BANK_HEADER.size or payload_bytes != len(data) - data_at:
        raise ValueError(f"{path}: invalid AFB payload range")
    digest = hashlib.sha256(data[data_at:]).digest()
    if (low, high) != struct.unpack_from("<2I", digest):
        raise ValueError(f"{path}: AFB identity does not match payload")
    return data[data_at:], low, high


def _read_flow(path: Path, flow_id: int | None = None) -> dict:
    data = path.read_bytes()
    if len(data) < AFX_HEADER.size:
        raise ValueError(f"{path}: truncated AFX header")
    header = AFX_HEADER.unpack_from(data)
    (magic, version, total, _, image_at, image_size, stream_at, stream_size,
     control_id, setup_count, bank_low, bank_high, relocations_at, relocation_count,
     reserved0, reserved1, channels, tick_num, tick_den, reserved) = header
    if (magic, version, total, reserved) != (AFX_MAGIC, AFX_VERSION, len(data), 0):
        raise ValueError(f"{path}: expected final bank-bound AFX")
    if not tick_num or not tick_den or channels > 64 or image_at > len(data) or image_size != len(data) - image_at:
        raise ValueError(f"{path}: invalid AFX image")
    if stream_at > image_size or stream_size > image_size - stream_at:
        raise ValueError(f"{path}: invalid AFX stream")
    if relocation_count != setup_count or relocations_at != AFX_HEADER.size or \
       relocations_at + relocation_count * AFX_RELOCATION.size > image_at:
        raise ValueError(f"{path}: invalid AFX relocations")
    if not control_id or reserved0 or reserved1:
        raise ValueError(f"{path}: AFX embeds deprecated metadata")
    if setup_count * SETUP_BYTES > image_size or stream_at < setup_count * SETUP_BYTES:
        raise ValueError(f"{path}: invalid AFX setup layout")
    relocations = []
    seen = set()
    for index in range(relocation_count):
        pair, offset, size = AFX_RELOCATION.unpack_from(data, relocations_at + index * AFX_RELOCATION.size)
        if pair % SETUP_BYTES or pair // SETUP_BYTES >= setup_count or pair in seen or not size:
            raise ValueError(f"{path}: invalid AFX relocation")
        seen.add(pair)
        relocations.append((pair, offset, size))
    return {"id": flow_id if flow_id is not None else _flow_id(path), "path": path, "data": data,
            "header": header, "bank_id": (bank_low, bank_high), "relocations": relocations}


def _read_seek(path: Path, header: tuple[int, ...]) -> bytes:
    if not path.is_file():
        return b""
    data = path.read_bytes()
    if len(data) < AFC_HEADER.size:
        raise ValueError(f"{path}: truncated AFC header")
    magic, version, control_id, bank_low, bank_high, data_at, data_size, total = AFC_HEADER.unpack_from(data)
    if (magic, version, control_id, bank_low, bank_high, total) != \
       (AFC_MAGIC, 1, header[8], header[10], header[11], len(data)) or \
       data_at != AFC_HEADER.size or not data_size or data_size != len(data) - data_at:
        raise ValueError(f"{path}: AFC does not match its AFX")
    return data[data_at:]


def _seek_file(header: list[int], checkpoints: bytes) -> bytes:
    if not checkpoints:
        return b""
    result = bytearray(AFC_HEADER.size + len(checkpoints))
    AFC_HEADER.pack_into(result, 0, AFC_MAGIC, 1, header[8], header[10], header[11],
                         AFC_HEADER.size, len(checkpoints), len(result))
    result[AFC_HEADER.size:] = checkpoints
    return bytes(result)


def _control_id(header: list[int], data: bytes) -> int:
    digest = hashlib.sha256(struct.pack("<5I", header[3], header[16], header[17],
                                        header[10], header[11]) +
                            data[header[12]:header[12] + header[13] * AFX_RELOCATION.size] +
                            data[header[4]:]).digest()
    return struct.unpack_from("<I", digest)[0] or 1


def _rewrite_checkpoints(data: bytes, ranges: list[tuple[int, int, int]]) -> bytes:
    """Relocate bank addresses in compiler checkpoint voice state."""
    if not data:
        return data
    if len(data) < 16 or struct.unpack_from("<4I", data)[:2] != (CHECKPOINT_MAGIC, 1):
        raise ValueError("invalid AFX checkpoint table")
    count, reserved = struct.unpack_from("<2I", data, 8)
    if reserved:
        raise ValueError("invalid AFX checkpoint table")
    result = bytearray(data)
    cursor = 16
    for _ in range(count):
        if cursor + 16 > len(result):
            raise ValueError("truncated AFX checkpoint")
        states = struct.unpack_from("<I", result, cursor + 12)[0]
        cursor += 16
        for _ in range(states):
            if cursor + 40 > len(result):
                raise ValueError("truncated AFX checkpoint state")
            control, low = struct.unpack_from("<HH", result, cursor + 4)
            if not control & 0x400:
                old = ((control & 0x7F) << 16) | low
                match = next(((start, size, new) for start, size, new in ranges if start <= old < start + size), None)
                if match is None:
                    raise ValueError("checkpoint refers outside its source bank")
                start, _, new = match
                address = new + old - start
                if address > 0x7FFFFF:
                    raise ValueError("checkpoint AICA address exceeds 23 bits")
                struct.pack_into("<HH", result, cursor + 4, (control & ~0x7F) | (address >> 16), address & 0xFFFF)
            cursor += 40
    if cursor != len(result):
        raise ValueError("invalid AFX checkpoint table size")
    return bytes(result)


def merge_bank_flows(paths: list[Path], flow_ids: dict[Path, int] | None = None) -> tuple[bytes, dict[int, bytes], dict[int, bytes], dict]:
    """Deduplicate final input banks and rebind their already-final AFX flows."""
    flow_ids = flow_ids or {}
    flows = [_read_flow(path, flow_ids.get(path)) for path in paths]
    for flow in flows:
        flow["checkpoints"] = _read_seek(flow["path"].with_suffix(".afc"), flow["header"])
    if len({flow["id"] for flow in flows}) != len(flows):
        raise ValueError("duplicate flow id")
    source_banks: dict[Path, tuple[bytes, int, int]] = {}
    samples: list[bytes] = []
    offsets: list[int] = []
    sample_by_data: dict[bytes, int] = {}
    plans = []
    for flow in sorted(flows, key=lambda item: item["id"]):
        bank_path = flow["path"].with_suffix(".afb")
        bank = source_banks.setdefault(bank_path, _read_bank(bank_path))
        payload, low, high = bank
        if flow["bank_id"] != (low, high):
            raise ValueError(f"{flow['path']}: AFX bank identity does not match {bank_path}")
        local = []
        for pair, offset, size in flow["relocations"]:
            if offset > len(payload) or size > len(payload) - offset:
                raise ValueError(f"{flow['path']}: relocation lies outside {bank_path}")
            raw = payload[offset:offset + size]
            sample = sample_by_data.setdefault(raw, len(samples))
            if sample == len(samples):
                samples.append(raw)
            local.append((pair, offset, size, sample))
        plans.append((flow, local))
    cursor = 0
    for sample in samples:
        cursor = align(cursor)
        offsets.append(cursor)
        cursor += len(sample)
    payload = bytearray(cursor)
    for sample, offset in zip(samples, offsets):
        payload[offset:offset + len(sample)] = sample
    digest = hashlib.sha256(payload).digest()
    bank_low, bank_high = struct.unpack_from("<2I", digest)
    bank = bytearray(BANK_HEADER.size + len(payload))
    BANK_HEADER.pack_into(bank, 0, AFB_MAGIC, AFB_VERSION, bank_low, bank_high,
                          BANK_HEADER.size, len(payload), len(bank), 0)
    bank[BANK_HEADER.size:] = payload
    controls = {}
    indices = {}
    for flow, local in plans:
        result = bytearray(flow["data"])
        header = list(flow["header"])
        image_at, relocations_at = header[4], header[12]
        ranges = []
        for index, (pair, old, size, sample) in enumerate(local):
            new = offsets[sample]
            if new > 0x7FFFFF:
                raise ValueError("AFB exceeds AICA's 23-bit address range")
            control, _ = struct.unpack_from("<HH", result, image_at + pair)
            struct.pack_into("<HH", result, image_at + pair, (control & ~0x7F) | (new >> 16), new & 0xFFFF)
            AFX_RELOCATION.pack_into(result, relocations_at + index * AFX_RELOCATION.size, pair, new, size)
            ranges.append((old, size, new))
        header[10:12] = bank_low, bank_high
        checkpoints = _rewrite_checkpoints(flow["checkpoints"], ranges)
        header[8] = _control_id(header, result)
        AFX_HEADER.pack_into(result, 0, *header)
        controls[flow["id"]] = bytes(result)
        seek = _seek_file(header, checkpoints)
        if seek: indices[flow["id"]] = seek
    diagnostics = {"flows": len(controls), "samples": len(samples),
                   "input_bank_bytes": sum(len(payload) for payload, _, _ in source_banks.values()),
                   "sample_bytes": len(payload), "flow_bytes": sum(map(len, controls.values())),
                   "total_bytes": len(bank)}
    return bytes(bank), controls, indices, diagnostics


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--controls-dir", type=Path, required=True)
    parser.add_argument("--control-name", help="output basename when merging one flow")
    parser.add_argument("--id", type=int, help="flow id when merging one non-sequence source")
    parser.add_argument("flow", type=Path, nargs="+")
    args = parser.parse_args(argv)
    try:
        if args.control_name and (len(args.flow) != 1 or Path(args.control_name).name != args.control_name):
            raise ValueError("--control-name needs one flow and a plain filename")
        if args.id is not None and (len(args.flow) != 1 or not 1 <= args.id <= 65535):
            raise ValueError("--id needs one flow and a value in 1..65535")
        ids = {args.flow[0]: args.id} if args.id is not None else None
        bank, controls, indices, diagnostics = merge_bank_flows(args.flow, ids)
        args.controls_dir.mkdir(parents=True, exist_ok=True)
        for ident, control in controls.items():
            (args.controls_dir / (args.control_name or f"sequence_{ident}.afx")).write_bytes(control)
            seek = indices.get(ident)
            if seek:
                (args.controls_dir / (args.control_name or f"sequence_{ident}.afx")).with_suffix(".afc").write_bytes(seek)
        args.output.write_bytes(bank)
    except (OSError, ValueError, struct.error) as error:
        print(f"afx-music-bank: {error}", file=sys.stderr)
        return 2
    print(f"wrote {args.output}: {diagnostics}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
