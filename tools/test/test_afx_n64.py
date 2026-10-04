#!/usr/bin/env python3
"""Basic command boundary check for the unified N64 reader."""
from pathlib import Path
import subprocess
import sys


TOOL = Path(__file__).resolve().parents[1] / "research/afx_n64.py"


def check(*arguments: str) -> None:
    result = subprocess.run([sys.executable, str(TOOL), *arguments], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


check("cseq", "--help")
check("audioseq", "--help")
