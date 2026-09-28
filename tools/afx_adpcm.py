"""AICA ADPCM encoders shared by AFX importers.

The default YA2BEAM encoder is adapted from SF64-DC's CC0 ya2beam.c.  A local
C build makes ordinary asset conversion fast; the deterministic Python fallback
keeps conversion available on hosts without a C compiler.
"""

from __future__ import annotations

import array
import ctypes
import functools
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import threading

_QUANT = (230, 230, 230, 230, 307, 409, 512, 614)
_SRC = Path(__file__).with_name("afx_ya2beam.c")
_LOCK = threading.Lock()
_LIBRARY: ctypes.CDLL | bool | None = None


def _host_compiler() -> str:
    """Return a native compiler, never the target compiler exported by a SDK."""
    return os.environ.get("AICAFLOW_HOST_CC", "cc")


def _trunc_div8(value: int) -> int:
    return value // 8 if value >= 0 else -((-value) // 8)


def _step(sample: int, quant: int, code: int) -> tuple[int, int]:
    delta = _trunc_div8(quant * (1 + 2 * (code & 7)))
    if code & 8:
        delta = -delta
    sample = max(-32768, min(32767, sample + delta))
    quant = max(0x7F, min(0x6000, quant * _QUANT[code & 7] >> 8))
    return sample, quant


def _fallback(samples: array.array[int], width: int) -> bytes:
    beams: list[tuple[int, int, int]] = [(0, 0, 0x7F)]
    back: list[list[tuple[int, int]]] = []
    for target in samples:
        choices: list[tuple[int, int, int, int, int]] = []
        for parent, (score, sample, quant) in enumerate(beams):
            for code in range(16):
                current, next_quant = _step(sample, quant, code)
                choices.append((score + (target - current) ** 2, current, next_quant, parent, code))
        choices.sort(key=lambda choice: choice[0])
        next_beams, parents, seen = [], [], set()
        for score, current, quant, parent, code in choices:
            if (current, quant) in seen:
                continue
            seen.add((current, quant))
            next_beams.append((score, current, quant))
            parents.append((parent, code))
            if len(next_beams) == width:
                break
        beams = next_beams
        back.append(parents)
    index = min(range(len(beams)), key=lambda item: beams[item][0])
    codes = [0] * len(samples)
    for position in range(len(samples) - 1, -1, -1):
        index, codes[position] = back[position][index]
    packed = bytearray((len(codes) + 1) // 2)
    for index, code in enumerate(codes):
        packed[index // 2] |= code << (4 * (index & 1))
    return bytes(packed)


def _library() -> ctypes.CDLL | None:
    global _LIBRARY
    if _LIBRARY is not None:
        return _LIBRARY if _LIBRARY is not False else None
    with _LOCK:
        if _LIBRARY is not None:
            return _LIBRARY if _LIBRARY is not False else None
        try:
            digest = hashlib.sha256(_SRC.read_bytes()).hexdigest()[:16]
            output = Path(tempfile.gettempdir()) / f"aicaflow-ya2beam-{digest}.so"
            if not output.exists():
                temporary = output.with_suffix(f".tmp-{os.getpid()}")
                subprocess.run([_host_compiler(), "-O3", "-shared", "-fPIC",
                                "-o", str(temporary), str(_SRC)], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                os.replace(temporary, output)
            library = ctypes.CDLL(str(output))
            library.ya2beam_encode.restype = ctypes.c_int
            library.ya2beam_encode.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                                                ctypes.c_int, ctypes.POINTER(ctypes.c_uint8)]
            library.n64_vadpcm_decode.restype = ctypes.c_int
            library.n64_vadpcm_decode.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int,
                                                   ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                                                   ctypes.POINTER(ctypes.c_int16)]
            _LIBRARY = library
        except Exception:
            _LIBRARY = False
        return _LIBRARY if _LIBRARY is not False else None


def pcm16_to_adpcm(raw: bytes, beam_width: int = 32) -> bytes:
    """Encode little-endian PCM16 using full-buffer YA2BEAM search."""
    if len(raw) & 1:
        raise ValueError("ADPCM source must contain whole PCM16 frames")
    if not 1 <= beam_width <= 256:
        raise ValueError("YA2BEAM width must be in 1..256")
    return _pcm16_to_adpcm_cached(raw, beam_width)


@functools.cache
def _pcm16_to_adpcm_cached(raw: bytes, beam_width: int) -> bytes:
    """Avoid re-encoding one immutable sample for every note that uses it."""
    samples = array.array("h")
    samples.frombytes(raw)
    if samples.itemsize != 2:
        raise RuntimeError("host int16 representation is unsupported")
    if not samples:
        return b""
    if os.sys.byteorder != "little":
        samples.byteswap()
    library = _library()
    if library is None:
        return _fallback(samples, beam_width)
    output = (ctypes.c_uint8 * ((len(samples) + 1) // 2))()
    source = (ctypes.c_int16 * len(samples)).from_buffer(samples)
    if library.ya2beam_encode(source, len(samples), beam_width, output):
        return _fallback(samples, beam_width)
    return bytes(output)


def n64_vadpcm_decode(raw: bytes, coefficients: list[int], predictors: int) -> bytes | None:
    """Native N64 VADPCM decode, or ``None`` when the C helper cannot run."""
    frames = len(raw) // 9
    if not frames:
        return None
    library = _library()
    if library is None:
        return None
    source = (ctypes.c_uint8 * (frames * 9)).from_buffer_copy(raw[:frames * 9])
    book = (ctypes.c_int16 * len(coefficients))(*coefficients)
    output = (ctypes.c_int16 * (frames * 16))()
    if library.n64_vadpcm_decode(source, frames * 9, book, predictors, output):
        return None
    return bytes(output)
