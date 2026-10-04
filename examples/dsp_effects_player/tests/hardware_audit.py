"""Record dry/wet pairs through the resident tuner (macOS, Swift and numpy).

Run from the repository root: python3 examples/dsp_effects_player/tests/hardware_audit.py OUTPUT_DIR
The Dreamcast must already run the tuner. Audio device is Live Gamer HDMI.
Metrics flag weak differences; they are not a substitute for listening.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import struct
import sys
import threading
import time
import wave

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/tuner"))
import client

HOST = "10.0.0.184"
RATE = 48000


def request(opcode, payload=b""):
    try:
        result, data = client.request(HOST, opcode, payload)
    except OSError as error:
        raise RuntimeError(f"tuner opcode {opcode}: {error}") from error
    if result:
        raise RuntimeError(f"tuner opcode {opcode}: {result}")
    return data


def upload(data, commit):
    result, _ = client.upload_staged(HOST, data, commit)
    if result:
        raise RuntimeError(f"upload {commit}: {result}")


def metrics(samples):
    mono = samples.mean(axis=1)
    frames = mono[:len(mono) // 2048 * 2048].reshape(-1, 2048)
    spectrum = np.mean(abs(np.fft.rfft(frames * np.hanning(2048), axis=1)) ** 2, axis=0)
    envelope = np.sqrt(np.mean(frames ** 2, axis=1))
    return spectrum, envelope


def main():
    output = Path(sys.argv[1]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    library = output / "dsp.dylib"
    subprocess.run(["cc", "-shared", "-fPIC", "-I", str(ROOT / "driver/sh4/include"),
                    "-I", str(ROOT / "driver/include"), "-I", str(ROOT / "driver/format/include"),
                    str(ROOT / "driver/sh4/src/dsp.c"), str(ROOT / "driver/sh4/src/dsp_prefabs.c"),
                    "-lm", "-o", str(library)], check=True)
    dsp = ctypes.CDLL(str(library))
    recorder = output / "capture"
    subprocess.run(["swiftc", str(Path(__file__).with_name("capture.swift")), "-o", str(recorder)], check=True)
    source = (ROOT / "examples/dsp_effects_player/code/main.c").read_text()
    effects = re.findall(r'\{"[^"]+", "([^"]+)", (\w+)\}', source)
    sources = dict(CONTROL_PHRASE="effect", IMPULSE="impulse", TONE="tone", MODULATED="modulated", WILHELM="wilhelm", SLOW="slow")
    arguments = sys.argv[2:]
    wet_only = "--wet-only" in arguments
    arguments = [arg for arg in arguments if arg != "--wet-only"]
    baseline = json.loads(Path(__file__).with_name("hardware-results.json").read_text())["baseline"]
    if arguments == ["--all"]:
        effects = [(item["preset"], item["source"]) for item in baseline]
    elif arguments:
        defaults = {item["preset"]: item["source"] for item in baseline}
        defaults.update(effects)
        effects = [(arg.split(":")[0], arg.split(":")[1] if ":" in arg else defaults[arg]) for arg in arguments]
    if not effects:
        raise ValueError("No matching demo effects")
    assets = Path(os.environ.get("AFX_AUDIT_ASSETS", ROOT / "examples/dsp_effects_player/cdrom/dsp_effects_player"))
    # Snapshot matched files before testing; a concurrent rebuild must not mix banks.
    bank = (assets / "inputs.afb").read_bytes()
    flows = {sources.get(source, source): (assets / (sources.get(source, source) + ".afx")).read_bytes()
             for _, source in effects}
    for stem, flow in flows.items():
        client.validate_control(flow)
        assert flow[40:48] == bank[8:16], f"{stem}: AFX/AFB identity mismatch"
    chunks = []
    with (output / "capture.log").open("w") as log:
        capture = subprocess.Popen([str(recorder)],
                                   stdout=subprocess.PIPE, stderr=log)
        def read_audio():
            while True:
                block = capture.stdout.read(4096)
                if not block:
                    return
                chunks.append(block)
        reader = threading.Thread(target=read_audio, daemon=True)
        reader.start()
        report = []
        try:
            for _ in range(100):
                if chunks or capture.poll() is not None:
                    break
                time.sleep(.1)
            if not chunks:
                raise RuntimeError("No HDMI audio; inspect capture.log")
            for preset, input_id in effects:
                stem = sources.get(input_id, input_id)
                print(f"Auditing {preset} / {stem}", flush=True)
                # Each effect starts from fresh firmware, bank and DSP state.
                request(client.RESET)
                upload(bank, client.BANK_COMMIT)
                upload(flows[stem], client.CONTROL_COMMIT_PLAY)
                request(client.STOP)
                pair = []
                for wet in (False, True):
                    if wet and wet_only:
                        flow = bytearray(flows[stem])
                        image_at = struct.unpack_from("<I", flow, 16)[0]
                        setups = struct.unpack_from("<I", flow, 36)[0]
                        for setup in range(setups):
                            # AFX v7: 18 uint16 fields, DIRECT is field 9.
                            struct.pack_into("<H", flow, image_at + setup * 36 + 18, 0)
                        upload(flow, client.CONTROL_COMMIT_PLAY)
                        request(client.STOP)
                    program = (ctypes.c_uint16 * 706)()
                    assert dsp.afx_dsp_program_demo(program, preset.encode()) == 0
                    data = bytes(program)
                    assert request(client.DSP_PROGRAM, data) == data, "DSP readback mismatch"
                    request(client.DSP_RETURNS, bytes([wet]))
                    request(client.MESSAGE, client.message_payload(f"AUDIT {preset} / {stem} / {'WET' if wet else 'DRY'}"))
                    time.sleep(.2)
                    first = len(chunks)
                    request(client.PLAY)
                    time.sleep(4.7)
                    request(client.STOP)
                    status = struct.unpack("<7I", request(client.STATUS))
                    if status[1]:
                        raise RuntimeError(f"ARM7 playback error: {status}")
                    raw = b"".join(chunks[first:])
                    samples = np.frombuffer(raw, dtype="<i2").reshape(-1, 2).astype(float) / 32768
                    if len(samples) < RATE * 4:
                        raise RuntimeError("Incomplete audio capture")
                    name = f"{preset}-{'wet' if wet else 'dry'}.wav"
                    with wave.open(str(output / name), "wb") as file:
                        file.setparams((2, 2, RATE, 0, "NONE", "not compressed"))
                        file.writeframes(raw)
                    pair.append(samples)
                dry, wet = pair
                sd, ed = metrics(dry)
                sw, ew = metrics(wet)
                spectral_distance = float(np.sum(abs(sd / sd.sum() - sw / sw.sum())) / 2)
                rms = lambda x: float(np.sqrt(np.mean(x*x)))
                a, b = dry.mean(axis=1), wet.mean(axis=1)
                fft_size = 1 << (len(a) + len(b) - 2).bit_length()
                correlation = np.fft.irfft(np.fft.rfft(b, fft_size) * np.fft.rfft(a[::-1], fft_size), fft_size)
                correlation = np.max(abs(correlation[len(a)-1-RATE:len(a)+RATE])) / np.sqrt(np.sum(a*a)*np.sum(b*b))
                stereo = wet[:len(wet)//2048*2048].reshape(-1, 2048, 2)
                levels = np.sqrt(np.mean(stereo**2, axis=1))
                active = np.max(levels, axis=1) > np.max(levels)*.1
                balance = 20*np.log10(np.maximum(levels[active, 0], 1e-8)/np.maximum(levels[active, 1], 1e-8))
                result = dict(preset=preset, source=stem,
                              wet_only=wet_only,
                              bank_sha256=hashlib.sha256(bank).hexdigest(),
                              flow_sha256=hashlib.sha256(flows[stem]).hexdigest(),
                              program_sha256=hashlib.sha256(data).hexdigest(),
                              dry_reference=preset,
                              peak_db=round(float(20*np.log10(max(abs(wet).max(), 1e-9))), 2),
                              dry_peak_db=round(float(20*np.log10(max(abs(dry).max(), 1e-9))), 2),
                              rms_change_db=round(20*np.log10(max(rms(wet), 1e-9)/max(rms(dry), 1e-9)), 2),
                              normalized_residual=round(float(np.sqrt(max(0, 1-correlation**2))), 3),
                              stereo_balance_range_db=round(float(np.ptp(balance)), 2),
                              spectral_distance=round(spectral_distance, 4),
                              dry_active_frames=int(sum(ed > ed.max()*.1)),
                              wet_active_frames=int(sum(ew > ew.max()*.1)))
                report.append(result)
                (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
                print(json.dumps(result), flush=True)
        finally:
            capture.terminate()
            capture.wait(timeout=10)
            reader.join(timeout=2)
            try:
                request(client.STOP)
            except RuntimeError as error:
                print(f"Cleanup STOP failed: {error}", file=sys.stderr)


if __name__ == "__main__":
    main()
