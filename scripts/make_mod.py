"""Write KenshiRogue.mod into the installed mod folder, from the vanilla data.

RE_Kenshi loads a mod's plugin only when that mod is enabled in Kenshi's mod
list (data/mods.cfg), and a mod needs a .mod file to be listed at all
(checked 2026-11-23). The mod also carries the run's encounters:

  1-KenshiRogue.mod  "Kenshi Rogue: one starving bandit"   (fight 1)
  2-KenshiRogue.mod  "Kenshi Rogue: two starving bandits"  (fight 2)

Both are the vanilla "Starving bandit mob" squad with its numbers fixed: a
Hungry bandit as leader (the vanilla leader is the tougher "Hungry bandit
leader"), and 0 or 1 more. The plugin refers to them by these stringIds.

    python -I scripts/make_mod.py <Kenshi\\mods\\KenshiRogue>
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod  # noqa: E402

DEPENDENCIES = ["gamedata.base", "Newwworld.mod", "rebirth.mod", "Dialogue.mod"]
STARVING_MOB = ("1307-gamedata.base", "Starving bandit mob")
HUNGRY_BANDIT = ("1304-gamedata.base", "Hungry bandit")


def base(db, sid_and_name):
    sid, name = sid_and_name
    rec = db[sid.encode()]
    if kmod.text(rec.name) != name:
        raise SystemExit("%s is %r, expected %r: the game data changed" % (sid, kmod.text(rec.name), name))
    return rec


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        raise SystemExit("usage: make_mod.py <installed KenshiRogue mod folder>")
    db = kmod.Database()
    mob = base(db, STARVING_MOB)
    bandit = base(db, HUNGRY_BANDIT)
    f = kmod.ModFile(16, description="Kenshi Rogue: a roguelite run through Kenshi's combat.",
                     dependencies=DEPENDENCIES)

    for count, name in ((1, "Kenshi Rogue: one starving bandit"), (2, "Kenshi Rogue: two starving bandits")):
        sid = f.new_id("KenshiRogue.mod")
        rec = kmod.clone(mob, sid, name)
        rec.id = f.last_id
        rec.refs[b"leader"] = [(bandit.sid, 1, 0, 0)]
        if count > 1:
            rec.refs[b"squad"] = [(bandit.sid, count - 1, count - 1, 0)]
        else:
            rec.refs.pop(b"squad", None)
        f.add(rec)

    path = os.path.join(sys.argv[1], "KenshiRogue.mod")
    kmod.write(f, path)
    assert kmod.dumps(kmod.read(path)) == kmod.dumps(f), path
    print("wrote %s (%d records)" % (path, len(f.records)))


if __name__ == "__main__":
    main()
