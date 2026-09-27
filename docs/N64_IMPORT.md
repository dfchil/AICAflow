# AICAflow DKR Edition: N64 import

DKR's imported audio is generated offline from extracted Nintendo 64 assets.
No game assets are shipped by AICAflow.

Music input is an N64 ALBank control file, its sample table and an S1 compact
sequence file:

```sh
python3 tools/afx_n64.py bank.ctl bank.tbl sequences.bin 40 sequence_40.afx
```

The output `.afx` files are intermediate music flows. Pack all sequences into
one AFB1 sample bank plus compact AFC1 controls:

```sh
python3 tools/afx_adpcm_audit.py build/dc/aicaflow audit.json
python3 tools/afx_music_bank.py build/dc/aicaflow/music.afb audit.json \
  --controls-dir build/dc/aicaflow/music_controls build/dc/aicaflow/sequence_*.afx
```

SFX input is the N64 ALBank control/sample pair. Generate a bank for explicit
N64 sound IDs with:

```sh
python3 tools/afx_n64_sfx.py bank.ctl bank.tbl output.afb 1 4 19
```

DKR's `dreamcast/build_aicaflow_sfx.py` and
`dreamcast/build_aicaflow_fallback.py` select the resident, scene, vehicle and
one-sound fallback banks. Use those scripts through `Makefile.dc`; their IDs
are derived from DKR assets rather than maintained by hand.

AFB1 stores shared samples and optionally SFX flows. AFC1 stores one music
control stream whose setups refer to samples already resident in an AFB1 bank.
Load the music samples once with `afx_sfx_bank_load_samples_file`, then upload
the desired AFC1 with `afx_sfx_bank_control_upload`.
