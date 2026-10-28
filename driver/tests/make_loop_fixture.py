#!/usr/bin/env python3
"""Write a looping flow for check_loop.c: the frozen quickstart fixture with its
END replaced by LOOP 0, so the three notes repeat without a gap.

    python3 driver/tests/make_loop_fixture.py driver/build

Writes loop.afx and loop.afb (the fixture's bank, unchanged) into the directory."""
import struct
import sys
from pathlib import Path

FIXTURES = Path(__file__).resolve().parent / "fixtures"
SIZES = {0: 1, 1: 2, 2: 3, 3: 5, 0x12: 2, 0x13: 1, 0x14: 8, 0x15: 4, 0x16: 5}


def control_id(image):
    value = 2166136261
    for byte in image:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value or 1


def events(stream):
    """(offset, opcode, size, wait, commands, writes) per instruction."""
    out, i = [], 0
    while i < len(stream):
        op = stream[i]
        if op in (0x10, 0x11):
            prefix = 8 if op == 0x10 else 6
            mask = struct.unpack_from("<I", stream, i + prefix - 4)[0]
            size = prefix + 2 * bin(mask).count("1")
            commands, writes = 1, 19 if op == 0x10 else bin(mask).count("1")
        else:
            size = SIZES[op]
            commands, writes = (1, 19) if op == 0x14 else (1, 1) if op in (0x12, 0x15) else (0, 0)
        wait = struct.unpack_from("<I", stream[i + 1:i + size] + b"\0" * 4)[0] if op in (1, 2, 3) else 0
        out.append((i, op, size, wait, commands, writes))
        i += size
    return out


def work_profile(stream):
    """Peak commands and register writes between waits, as the validator counts
    them; the LOOP joins the stream's last burst to the one at its target."""
    bursts, commands, writes = [], 0, 0
    for _, op, _, _, c, w in events(stream):
        if op in (1, 2, 3):
            bursts.append((commands, writes))
            commands = writes = 0
        commands += c
        writes += w
    head = next(((c, w) for c, w in [bursts[0]] if bursts), (0, 0))
    bursts.append((commands + head[0], writes + head[1]))
    return (max(c for c, _ in bursts) << 16) | max(w for _, w in bursts)


def main(out_dir):
    afx = bytes.fromhex((FIXTURES / "quickstart.afx.hex").read_text())
    afb = bytes.fromhex((FIXTURES / "quickstart.afb.hex").read_text())
    header = list(struct.unpack_from("<20I", afx))
    image_offset, image_size, stream_offset, stream_size = header[4:8]
    image = bytearray(afx[image_offset:image_offset + image_size])
    stream = image[stream_offset:stream_offset + stream_size]
    assert stream[-1] == 0, "the fixture's stream must end with END"
    image[stream_offset:stream_offset + stream_size] = bytes(stream[:-1]) + bytes([0x16]) + struct.pack("<I", 0)
    header[2] = image_offset + len(image)   # total_size
    header[3] |= 32                         # AFX_FLAG_LOOP
    header[5] = len(image)                  # image_size
    header[7] = stream_size + 4             # stream_size: END (1) -> LOOP (5)
    header[8] = control_id(image)
    header[14] = 0                          # loop_tick: the target is the stream start
    header[19] = work_profile(image[stream_offset:stream_offset + stream_size + 4])  # trusted loaders need it
    out = bytearray(afx[:image_offset])
    struct.pack_into("<20I", out, 0, *header)
    out += image
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "loop.afx").write_bytes(out)
    (out_dir / "loop.afb").write_bytes(afb)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "driver/build")
