"""Require matching, periodic seek data for the three long demo songs."""
from pathlib import Path
import struct
import sys


def check(flow):
    header = struct.unpack_from("<20I", flow.read_bytes())
    data = flow.with_suffix(".afc").read_bytes()
    magic, version, control, low, high, offset, size, total = struct.unpack_from("<8I", data)
    assert magic == 0x00434641 and version == 1
    assert (control, low, high) == (header[8], header[10], header[11])
    assert offset == 32 and size == len(data) - offset and total == len(data)
    magic, version, count, reserved = struct.unpack_from("<4I", data, offset)
    assert magic == 0x31504B43 and version == 1 and reserved == 0
    assert count > 1, "start-only AFC: regenerate with the updated AICAforge checkpoint tool"
    interval = (10 * header[17] + header[18] - 1) // header[18]
    cursor = offset + 16
    for index in range(count):
        tick, position, remaining, channels = struct.unpack_from("<4I", data, cursor)
        assert tick == index * interval, "expected checkpoints every 10 authored seconds"
        assert header[6] <= position < header[6] + header[7]
        assert 0 <= channels <= header[16]
        cursor += 16 + channels * 40
        assert cursor <= len(data)
    assert cursor == len(data)
    print(f"{flow.stem}: {count} seek checkpoints, 10-second spacing")


if __name__ == "__main__":
    for argument in sys.argv[1:]:
        try:
            check(Path(argument))
        except (AssertionError, OSError, struct.error, ZeroDivisionError) as error:
            raise SystemExit(f"{argument}: invalid seek data: {error}")
