#!/usr/bin/env python3
"""Copies a C file with each `#embed "path"` replaced by the file's bytes, for
compilers without C23 #embed (CI's clang 18). Paths resolve as #embed's do,
against the source file's directory.

    python3 expand_embed.py source.c out.c"""
import re
import sys
from pathlib import Path

source, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
lines = []
for line in source.read_text().splitlines():
    match = re.match(r'\s*#embed\s+"([^"]+)"\s*$', line)
    if not match:
        lines.append(line)
        continue
    data = (source.parent / match.group(1)).read_bytes()
    for start in range(0, len(data), 24):
        lines.append(",".join(str(b) for b in data[start:start + 24]) + ",")
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text("\n".join(lines) + "\n")
