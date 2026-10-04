# Dynamic SFX

[Documentation](../../docs/README.md)

This self-contained Dreamcast demo runs a dRxLaX engine loop. Its engine stays inside the stereo field (pan 5 through 26), while
its intensity, pitch, filter resonance, low-pass brightness and triangle LFO
change every 16 ms through calls from the SH-4. A dRxLaX slide starts on the right and crosses to
the left over its 1.4-second playback; its complete pan sequence is packed into
its AFX stream before activation. It repeats after a 300 ms pause and alternates
direction on each slide.

The demo runs until the player exits it: **A** selects the engine loop, **B**
selects the authored slide sequence, and **START** returns to `kos-load`.
Only the selected effect is audible.

The analog stick's horizontal axis controls engine pan; its vertical axis controls
distance (level and low-pass brightness). The right trigger is throttle, changing
pitch, level and filter-Q; the left trigger brakes, reducing pitch, level and
brightness. LFO depth increases toward idle. If no controller is connected, the
engine follows the automatic demonstration motion.

The enDjinn display uses sprites to show both pan rails: blue for the engine
and orange for the authored slide. It also shows the current engine throttle and
pitch as bars, plus the logical slide-pan value and whether its sequence is
still playing.

```sh
source /opt/toolchains/dc/kos/environ.sh
make -C examples/dynamic_sfx
make -C examples/dynamic_sfx bin/aicaflow_dynamic_sfx.cdi
```

The engine's `update_engine()` in [`code/main.c`](code/main.c) patches a running
controlled loop, which ends its authored stream with `PARK`. The slide sequence
instead contains `NOTE`, timed `PATCH DIRECT` commands, `KEYOFF` and `END`; after
it is activated, the SH-4 sends no `afx_instance_patch()` commands for that effect.

The demo's pan value is 0 (left) through 31 (right). Its helper converts it to
AICA's direction/attenuation encoding: 0x1f is left, 0x10/0x00 centre and 0x0f right.
`MIX` uses the upper byte as total-level attenuation, so lower attenuation means
a louder sound. `PITCH` is AICA's native octave/FNS word; the demo sweeps
`0x0080` through `0x03ff`, approximately one to two times the source rate.
`DIRECT` bits 8–11 hold direct output level, fixed at 15. `MIX` bits 0–4
hold filter Q; bits 5 and 6 stay clear to enable the filter and attenuation.
Brightness 0–15 maps to cutoff words 0x1500–0x1e00 in all five filter-envelope
levels. Envelope rates are 31 so live cutoff changes reach the sustained loop.
Engine TL includes 24 steps of headroom for resonance.
The engine also uses a conservative triangle LFO
(rate 6, pitch depth 1–3, amplitude depth 1–2). All of these fields are sent in
one ascending-order register patch every 16 ms.

The bundled PCM16 assets are dRxLaX's `thrust_red.wav` and `mine_slide.wav`.
Both have four copied guard frames; the engine loops and the slide plays once.
At startup the demo places them in one contiguous AFB payload, then creates its
three runtime-authored sample-free AFX flows (engine and two slide directions)
against that bank. Update only the
fields that actually changed in the game tick.
