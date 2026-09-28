# DKR Music Player

Dreamcast player for the 64 non-silent Diddy Kong Racing tracks. It is a
self-contained AICAflow example: one checked-in shared `music.afb`, compact
bank-bound `.afx` flows, optional `.afc` seek indexes, and VIZ1 spectrum data.
The player needs no DKR checkout or source extraction to build.

The D-pad selects, A plays or pauses, B stops, LEFT and RIGHT seek ten
seconds, and the triggers page through the list. `START+A+B+X+Y` is enDjinn's
normal soft-reset exit chord. Short cues appear last; ambient tracks sit just
above them.

Build with an [enDjinn](https://github.com/dfchil/enDjinn) checkout beside
AICAflow, or beside the DKR checkout when AICAflow is its submodule:

```sh
# Source the environ.sh from that enDjinn checkout.
make
make check
make bin/dkr_music_player.cdi
```

Run it through dc-load-ip:

```sh
dc-tool-ip -f -t "$DCTOOL_HOST" \
  -m "$PWD/cdrom/dkr_music_player" \
  -x "$PWD/bin/dkr_music_player.elf"
```

See [assets/README.md](assets/README.md) for the checked-in asset layout.
