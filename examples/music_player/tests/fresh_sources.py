"""Check the real Makefile's download dependencies without network or KOS."""
from pathlib import Path
import shutil
import subprocess
import tempfile

example = Path(__file__).resolve().parents[1]
names = ("chopin-op27-no1.mid", "grieg-mountain-king-orchestra.mid",
         "GeneralUser-GS.sf2", "cello_solo.sf2")
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    player = root / "examples/music_player"
    player.mkdir(parents=True)
    framework = root / "dependencies/enDjinn"
    framework.mkdir(parents=True)
    (framework / "base_link.mk").touch()
    (player.parent / "authoring.mk").touch()
    shutil.copyfile(example / "Makefile", player / "Makefile")
    (player / "fetch_assets.sh").write_text(
        "set -eu\nsleep 0.1\nmkdir -p sources\n"
        + "".join(f"printf fixture > sources/{name}\n" for name in names)
    )
    subprocess.run(["make", "-j8", *("sources/" + n for n in names)],
                   cwd=player, check=True)
    assert all((player / "sources" / n).stat().st_size for n in names)
print("Fresh parallel source download dependencies passed")
