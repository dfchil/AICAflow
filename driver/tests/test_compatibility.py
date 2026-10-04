"""Frozen older assets and optional independently built current authoring tools."""
import argparse
import hashlib
import json
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def validate(prefix):
    bank, flow, seek = (prefix.with_suffix(ext).read_bytes()
                        for ext in (".afb", ".afx", ".afc"))
    b, f, s = (struct.unpack_from("<8I", bank), struct.unpack_from("<20I", flow),
               struct.unpack_from("<8I", seek))
    assert b[0:2] == (0x00424641, 1) and b[4] == 32
    assert b[4] + b[5] == b[6] == len(bank) and b[7] == 0
    assert f[0:2] == (0x32584641, 7) and f[2] == len(flow)
    assert f[4] % 32 == 0 and f[4] + f[5] == len(flow)
    assert b[2:4] == f[10:12] == s[3:5]
    assert s[0:2] == (0x00434641, 1) and s[2] == f[8]
    assert s[5] == 32 and s[5] + s[6] == s[7] == len(seek)
    subprocess.run([str(ROOT / "driver/build/afx_validate"), str(prefix.with_suffix(".afx"))],
                   check=True)
    subprocess.run([str(ROOT / "driver/build/test_bank"),
                    *(str(prefix.with_suffix(ext)) for ext in (".afb", ".afx", ".afc"))],
                   check=True)
    invalid = prefix.with_suffix(".invalid.afx")
    invalid.write_bytes(flow[:4] + struct.pack("<I", 0) + flow[8:])
    assert subprocess.run([str(ROOT / "driver/build/afx_validate"), str(invalid)],
                          capture_output=True).returncode == 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--authoring-bin", type=Path)
    args = parser.parse_args()
    manifest = json.loads((ROOT / "firmware/manifest.json").read_text())
    assert hashlib.sha256((ROOT / "firmware" / manifest["file"]).read_bytes()).hexdigest() == manifest["sha256"]
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        for ext in ("afb", "afx", "afc"):
            source = ROOT / f"driver/tests/fixtures/quickstart.{ext}.hex"
            (root / f"older.{ext}").write_bytes(bytes.fromhex(source.read_text()))
        validate(root / "older")
        if args.authoring_bin:
            for name in ("current", "repeat"):
                subprocess.run([str(args.authoring_bin.resolve() / "afx_demo_assets"),
                                "quickstart", str(root / f"{name}.afb"),
                                str(root / f"{name}.afx")], check=True)
                validate(root / name)
            for ext in ("afb", "afx", "afc", "afv"):
                assert (root / f"current.{ext}").read_bytes() == (root / f"repeat.{ext}").read_bytes()
    print("Frozen/current format, bank binding, checkpoint and firmware checks passed")


if __name__ == "__main__":
    main()
