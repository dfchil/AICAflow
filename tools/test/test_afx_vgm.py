#!/usr/bin/env python3
"""Small self-contained MultiPCM VGM smoke test for the native importer."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile


TOOL = Path(os.environ.get("AFX_BUILD_DIR", Path(__file__).resolve().parents[1] / "build")) / "afx_vgm"


def write_register(data: bytearray, address: int, value: int) -> None:
    data.extend((0xB5, 2, address, 0xB5, 0, value))


def fixture() -> bytes:
    header = bytearray(0x8C)
    header[:4] = b"Vgm "
    struct.pack_into("<I", header, 8, 0x161)
    struct.pack_into("<I", header, 0x34, 0x58)  # data begins at 0x8c
    struct.pack_into("<I", header, 0x88, 8_000_000)  # one MultiPCM chip
    rom = bytearray(44)
    rom[5:7] = b"\xff\xe0"  # 32 decoded PCM8 frames
    rom[8:11] = b"\x11\x11\x11"
    rom[12:] = bytes(range(32))
    data = bytearray((0x67, 0x66, 0x89))
    data += struct.pack("<I", len(rom) + 8) + struct.pack("<II", len(rom), 0) + rom
    data += bytes((0xB5, 1, 0))  # select source slot zero
    write_register(data, 1, 0)   # sample ID, loads ROM defaults
    write_register(data, 2, 0)
    write_register(data, 3, 0x10)
    write_register(data, 0, 0x70)
    write_register(data, 4, 0x80)  # KEYON
    data += bytes((0x61, 0x44, 0xAC))
    write_register(data, 4, 0)     # KEYOFF
    data += b"\x66"
    return bytes(header + data)


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    source, output = root / "fixture.vgm", root / "fixture.afx"
    source.write_bytes(fixture())
    subprocess.run([str(TOOL), str(source), str(output)], check=True)
    afb, afc, afv = (output.with_suffix(suffix) for suffix in (".afb", ".afc", ".afv"))
    assert output.read_bytes()[:4] == b"AFX2"
    assert afb.read_bytes()[:4] == b"AFB\0"
    assert afc.read_bytes()[:4] == b"AFC\0"
    assert afv.read_bytes()[:4] == b"VIZ1"

print("native VGM importer fixture passed")
