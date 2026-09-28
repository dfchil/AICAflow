# DKR player assets

This directory is the fixed AICAflow input for the DKR music-player example.
It contains one shared `music.afb`, 64 compact `.afx` flows, their optional
`.afc` seek indexes, and 64 VIZ1 spectrum sidecars. `include/songs.h` is the
matching generated playlist metadata.

The example build copies these files unchanged into its CD/pc asset directory;
it does not need a DKR checkout, extracted game data, or a Python converter.
The DKR integration repository separately owns regeneration from its source
assets.
