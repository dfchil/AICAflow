#!/usr/bin/env python3
"""A source checkout uses its firmware without invoking ARM tools."""
from pathlib import Path
import shutil
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / "driver/arm7"
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for name in ("Makefile", "aicaflow.drv"):
        shutil.copyfile(source / name, root / name)
    original = (root / "aicaflow.drv").read_bytes()
    assert original
    def make(*targets):
        return subprocess.run(["make", *targets, "DC_ARM_CC=false", "DC_ARM_OBJCOPY=false"],
                              cwd=root, capture_output=True)
    for targets in ((), ("clean",), ()):
        result = make(*targets)
        assert result.returncode == 0, result.stderr.decode()
        assert (root / "aicaflow.drv").read_bytes() == original
    (root / "aicaflow.drv").unlink()
    result = make()
    assert result.returncode != 0 and b"Missing aicaflow.drv" in result.stderr
print("Prebuilt firmware: no ARM tools, clean preserves binary, missing binary fails clearly")
