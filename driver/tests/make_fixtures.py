#!/usr/bin/env python3
"""Write the console checks' fixtures from the frozen quickstart fixture (three
notes, 8160 ticks, one channel):

    python3 driver/tests/make_fixtures.py driver/build

- long.afx: the notes eight times over, then END: a plain flow that outlasts
  every check (about 65 s);
- song.afm: the notes as one section whose NEXT continues on the same
  executor and voices, played in a loop, as a LOOP flow did;
- song_new.afm: the same section with its NEXT 1000 ticks before the end and a
  new-play join, so each play rings out its last second on the song's other
  executor while the next one starts;
- long.afb: the fixture's bank, for all three."""
import struct
import sys
from pathlib import Path

FIXTURES = Path(__file__).resolve().parent / "fixtures"
SIZES = {0: 1, 1: 2, 2: 3, 3: 5, 0x12: 2, 0x13: 1, 0x14: 8, 0x15: 4, 0x19: 2, 0x1A: 2}
WAITS = (1, 2, 3)
NEXT, END, JOIN_NEW, JOIN_CONTINUE = 0x19, 0x00, 0, 1


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
            commands, writes = (1, 19) if op == 0x14 else (1, 1) if op in (0x12, 0x15, 0x1A) else (0, 0)
        wait = struct.unpack_from("<I", stream[i + 1:i + size] + b"\0" * 4)[0] if op in WAITS else 0
        if op == 1:
            wait &= 0xFF
        elif op == 2:
            wait &= 0xFFFF
        out.append((i, op, size, wait, commands, writes))
        i += size
    return out


def flow_profile(stream):
    """Peak commands and writes in a tick of a flow played once."""
    bursts, commands, writes = [], 0, 0
    for _, op, _, _, c, w in events(stream):
        if op in WAITS:
            bursts.append((commands, writes))
            commands = writes = 0
        commands += c
        writes += w
    bursts.append((commands, writes))
    return (max(c for c, _ in bursts) << 16) | max(w for _, w in bursts)


def song_profile(stream, body, join):
    """A one-section song looping to itself, played as the validator plays it
    (afx_song_validate): the peak work in a tick of both executors and the
    most pool voices held at once."""
    ev = events(stream)
    execs = [{"at": 0, "due": 0, "mapped": set(), "on": True}, {"on": False, "ended": -1}]
    execs[0]["ended"] = -1
    peak_c = peak_w = voices = joins = 0
    while joins < 4:
        now = min(x["due"] for x in execs if x["on"])
        commands = writes = 0
        busy = True
        while busy:
            busy = False
            for e, x in enumerate(execs):
                while x["on"] and x["due"] == now:
                    busy = True
                    _, op, _, wait, c, w = ev[x["at"]]
                    x["at"] += 1
                    if op in WAITS:
                        x["due"] += wait
                        break
                    commands += c
                    writes += w
                    if op in (0x10, 0x11, 0x12, 0x14, 0x15):
                        x["mapped"].add(0)
                    elif op == END:
                        x.update(on=False, ended=now, mapped=set())
                    elif op == NEXT:
                        joins += 1
                        if join == JOIN_CONTINUE:
                            x["at"] = 0
                        else:
                            other = execs[1 - e]
                            assert not other["on"] and other["ended"] != now
                            execs[1 - e] = {"at": 0, "due": now, "mapped": set(), "on": True, "ended": -1}
                    if join == JOIN_NEW:
                        voices = max(voices, sum(len(y["mapped"]) for y in execs if "mapped" in y))
        peak_c, peak_w = max(peak_c, commands), max(peak_w, writes)
    return (peak_c << 16) | peak_w, voices


def split(stream, tick):
    """The stream with a boundary at tick, splitting the WAIT across it."""
    out, now = bytearray(), 0
    for i, op, size, wait, _, _ in events(stream):
        if op in WAITS and now < tick < now + wait:
            out += bytes([2]) + struct.pack("<H", tick - now)
            rest = bytes([2]) + struct.pack("<H", now + wait - tick)
            return bytes(out), rest + bytes(stream[i + size:]), now + wait - tick
        out += stream[i:i + size]
        now += wait
        if now == tick:
            return bytes(out), bytes(stream[i + size:]), 0
    raise ValueError("tick past the stream")


def song(afx, stream, body, join, voices, profile):
    """An AFM of the fixture's setup and relocation and one section, order [0], loop 0."""
    header = struct.unpack_from("<20I", afx)
    image_offset, image_size, stream_offset = header[4:7]
    setups = afx[image_offset:image_offset + stream_offset]
    relocation = bytearray(afx[header[12]:header[12] + 12])
    struct.pack_into("<I", relocation, 0, 32)  # The setup now follows the image header.
    table = 32 + len(setups)
    order = table + 12
    streams = order + 4
    image = bytearray(32) + setups + struct.pack("<3I", streams, len(stream), body) + struct.pack("<HH", 0, 0)
    image += stream
    struct.pack_into("<8I", image, 0, 1, 1, 0, table, order, streams, len(stream), header[16])
    file = bytearray(128)
    file[96:108] = relocation
    struct.pack_into("<24I", file, 0, 0x314D4641, 1, 128 + len(image), 2, 128, len(image), header[9], 96, 0,
                     header[10], header[11], header[16], voices, header[17], header[18], profile,
                     control_id(image), 0, 0, 0, 0, 0, 0, 0)
    file[96:108] = relocation
    return bytes(file + image)


def main(out_dir):
    afx = bytes.fromhex((FIXTURES / "quickstart.afx.hex").read_text())
    afb = bytes.fromhex((FIXTURES / "quickstart.afb.hex").read_text())
    header = list(struct.unpack_from("<20I", afx))
    image_offset, image_size, stream_offset, stream_size = header[4:8]
    image = bytearray(afx[image_offset:image_offset + image_size])
    stream = bytes(image[stream_offset:stream_offset + stream_size])
    assert stream[-1] == END, "the fixture's stream must end with END"
    notes = stream[:-1]
    ticks = sum(wait for _, op, _, wait, _, _ in events(stream) if op in WAITS)

    # long.afx: eight passes, then END.
    long_stream = notes * 8 + bytes([END])
    long_image = image[:stream_offset] + long_stream
    long_header = list(header)
    long_header[2] = image_offset + len(long_image)
    long_header[5] = len(long_image)
    long_header[7] = len(long_stream)
    long_header[8] = control_id(long_image)
    long_header[19] = flow_profile(long_stream)
    out = bytearray(afx[:image_offset])
    struct.pack_into("<20I", out, 0, *long_header)

    # song.afm: continue join at the end; song_new.afm: new-play join 1000 ticks before it.
    continued = notes + bytes([NEXT, JOIN_CONTINUE, END])
    head, tail, _ = split(notes, ticks - 1000)
    renewed = head + bytes([NEXT, JOIN_NEW]) + tail + bytes([END])
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "long.afx").write_bytes(bytes(out + long_image))
    profile, _ = song_profile(continued, ticks, JOIN_CONTINUE)
    (out_dir / "song.afm").write_bytes(song(afx, continued, ticks, JOIN_CONTINUE, 0, profile))
    profile, voices = song_profile(renewed, ticks - 1000, JOIN_NEW)
    (out_dir / "song_new.afm").write_bytes(song(afx, renewed, ticks - 1000, JOIN_NEW, voices, profile))
    (out_dir / "long.afb").write_bytes(afb)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "driver/build")
