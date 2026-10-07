# Persistent tuner

[Documentation](README.md)

The tuner receives AFB/AFX assets and DSP programs over TCP while staying
resident on Dreamcast. Use it to audition songs, regions and live register
changes. Compile MIDI, SF2 and profiles on the host before upload.

Build and load it once:

```sh
source /opt/toolchains/dc/kos/environ.sh
make tools
kos-tool -t "$DCTOOL_HOST" -f \
  -x tools/tuner/server/bin/afx_tuner_server.elf
```

Keep this host process running, and use a second terminal for tuner commands.
Do not pass `-n`: with the tested KOS/dc-load combination, disabling the host
console changes the loader magic that KOS checks on exit. The tuner can stop
at `TUNER EXITED` without returning to the loader. That message confirms
AICAflow shutdown, not successful loader return.

The client requires Python 3 and no external packages:

```sh
python3 tools/tuner/client.py --host "$DCTOOL_HOST" ping
```

For a Python API example:

```sh
python3 tools/tuner/example.py music.afb title.afx --index title.afc --reset
```

It performs `ping`, optional hard reset, bank upload, control upload, optional
seek-index upload and a final `status` read. Add `--region START_MS DURATION_MS`
to immediately audition one passage.

## Normal workflow

An AFX is always a control flow bound to one AFB. Upload the shared bank once,
then replace small controls as often as needed. AFC is optional seek data that
remains in SH4 memory; it is attached after the control flow.

```sh
python3 tools/tuner/client.py --host "$DCTOOL_HOST" bank-upload --file music.afb
python3 tools/tuner/client.py --host "$DCTOOL_HOST" \
  control-upload-play --file title.afx --index title.afc
python3 tools/tuner/client.py --host "$DCTOOL_HOST" region \
  --start-ms 12000 --duration-ms 8000
python3 tools/tuner/client.py --host "$DCTOOL_HOST" stop
```

`play` restarts the current AFX. `gain --gain 0..255` changes its instance
gain. `patch`, `lane-mute`, `dsp-room`, `dsp-returns`, `dsp-enable` and
`dsp-disable` operate on the current running state. `status` reports the ARM7
instance state, timer, heartbeat and current AICA asset address. `message`
writes a short printable line to the Dreamcast screen.

For direct inspection, `asset-read --offset N --bytes N` reads up to 4096 bytes
of the current AFX image and `register-read --offset N --bytes N` reads aligned
AICA register words. `dsp-program` and `dsp-program-ring` upload a complete
DSP program and read it back; `dsp-readback` reads the active program.

## Memory lifetime and reset

The tuner owns exactly one current bank and one current control flow:

| Action | AICA/SH4 effect |
| --- | --- |
| `bank-upload` | Stops and frees the old AFX, releases the old AFB allocation, then loads the new AFB. |
| `control-upload-play` | Stops and frees the old AFX, but keeps the current AFB, then uploads and starts the new control flow. |
| `--index track.afc` | Keeps the AFB and AFX; replaces only the host-side seek index. |
| `stop` | Stops/recycles playback; keeps AFB and AFX for `play`, patching and inspection. |
| a staged upload | Uses a temporary SH4 buffer. It is freed after its commit, when a later upload begins, or on `reset`/`exit`. |

Freed AICA memory is reusable but not zeroed. Use `reset` to clear it.

To clear assets, uploads and playback state:

```sh
python3 tools/tuner/client.py --host "$DCTOOL_HOST" reset
```

This is a hard reset: the tuner disables AICAflow, calls `afx_init` again, and
therefore zeroes all 2 MiB of AICA RAM before re-uploading the ARM7 firmware.
It also drops the current AFB, AFX, AFC seek data, instance, DSP program,
runtime patches, lane mutes and staged SH4 upload. The tuner's default instance
gain returns to 255. Re-upload the bank and control flow afterwards. `exit`
instead terminates the tuner process; it is not the command to use between
auditions.

Verify a complete exit with:

```sh
python3 tools/tuner/client.py --host "$DCTOOL_HOST" exit
# Wait for the original kos-tool process to print "Program returned 0" and exit.
kos-tool -t "$DCTOOL_HOST" --ready
```

The reset covers the complete AICA RAM arena, including any optional DSP delay
ring. Its fixed and dynamic regions are documented in [Memory layout](memory.md).

## Protocol boundary

Each request is one TCP connection and has a fixed 16-byte little-endian
header: magic `AFT1`, protocol version, opcode, nonzero sequence and payload
size. The response repeats the opcode with bit 15 set and begins with a signed
AICAflow result. `ping` returns the highest supported opcode: 27 (`reset`).
Staged files are limited to 4 MiB; uploads use acknowledged 32 KiB chunks.
Each accepted connection uses a 4 KiB TCP send buffer to reduce SH4 memory
held during KOS's TIME_WAIT period. The receive buffer retains its KOS default.
Memory-backed sample-bank uploads use one reusable DMA buffer of at most
64 KiB in addition to the tuner's staged file, not a second bank-sized copy.
