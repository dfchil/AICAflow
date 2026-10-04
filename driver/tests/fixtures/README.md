# Pre-extraction compatibility fixture

The three hex files are frozen outputs of `afx_demo_assets quickstart`
built from AICAflow commit `7bfec64` (tag `pre-repo-split`).
The synthetic sine asset needs no external media. Hex encoding makes the
small binary baseline reviewable; tests decode it without rebuilding it.

AFB version 1, AFX version 7, AFC version 1. This baseline must not be
regenerated merely to make a failing compatibility check pass.
The test validates the files with the real codec and simulated SH4 loader,
including mismatched-bank and stale-checkpoint rejection.
