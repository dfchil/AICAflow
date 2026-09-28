# AICAflow DKR Edition: N64 import

DKR's imported audio is generated offline from extracted Nintendo 64 assets.
No game assets are shipped by AICAflow.

Music input is an N64 ALBank control file, its sample table and an S1 compact
sequence file:

```sh
python3 tools/afx_n64.py bank.ctl bank.tbl sequences.bin 40 sequence_40.afx
```

That writes one independently usable AFB plus sample-free AFX. Collection
builders may combine sources into one shared bank during their offline build;
that intermediate representation is not a published format or runtime input.

SFX input is the N64 ALBank control/sample pair. Generate a bank for explicit
N64 sound IDs with:

```sh
python3 tools/afx_n64_sfx.py bank.ctl bank.tbl output.afb 1 4 19
```

DKR's `dreamcast/build_aicaflow_sfx.py` and
`dreamcast/build_aicaflow_fallback.py` select the resident, scene, vehicle and
one-sound fallback banks. Use those scripts through `Makefile.dc`; their IDs
are derived from DKR assets rather than maintained by hand.

AFB stores only shared sample bytes. Each AFX stores its control stream,
playback registers and bank-relative sample offsets. Load the bank once with
`afx_bank_load_file`, then upload the desired AFX with `afx_bank_flow_upload`.
