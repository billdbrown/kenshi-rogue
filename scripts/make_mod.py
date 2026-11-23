"""Write KenshiRogue.mod, an empty data mod, into the installed mod folder.

RE_Kenshi loads a mod's plugin only when that mod is enabled in Kenshi's mod
list (data/mods.cfg), and a mod needs a .mod file to be listed at all
(checked 2026-11-23). The plugin carries no game data yet, so the file is
empty; it exists so the mod can be switched on and off like any other.

    python -I scripts/make_mod.py <Kenshi\\mods\\KenshiRogue>
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod  # noqa: E402


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        raise SystemExit("usage: make_mod.py <installed KenshiRogue mod folder>")
    path = os.path.join(sys.argv[1], "KenshiRogue.mod")
    f = kmod.ModFile(16, description="Kenshi Rogue")
    kmod.write(f, path)
    assert kmod.dumps(kmod.read(path)) == kmod.dumps(f), path
    print("wrote", path)


if __name__ == "__main__":
    main()
