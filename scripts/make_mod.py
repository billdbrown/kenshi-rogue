"""Write KenshiRogue.mod into the installed mod folder, from the vanilla data.

RE_Kenshi loads a mod's plugin only when that mod is enabled in Kenshi's mod
list (data/mods.cfg), and a mod needs a .mod file to be listed at all
(checked 2026-11-23). The mod also carries the run's encounters:

  1-KenshiRogue.mod  "Kenshi Rogue: one starving bandit"   (fight 1)
  2-KenshiRogue.mod  "Kenshi Rogue: two starving bandits"  (fight 2)

Both are the vanilla "Starving bandit mob" squad with its numbers fixed: a
Hungry bandit as leader (the vanilla leader is the tougher "Hungry bandit
leader"), and 0 or 1 more.

  3..5-KenshiRogue.mod  the ankh's material, part and worship
  6-KenshiRogue.mod     "Kenshi Rogue: ankh", a building you bow at to leave a
                        node (Bill, 2026-11-23). A placeholder: Kenshi Town's
                        Faience Ankh model (its scripts/assets/build_ankh.cmd
                        builds the files; install copies them from there, so
                        Bill's model isn't republished in this repo). Its use is
                        a throne's, with the fogmen's bowing for sitting, as in
                        Kenshi Town.

  7..12-KenshiRogue.mod  the encounter pool past the first fight (see the
                        plugin's ENCOUNTERS): a hungry bandit leader, a dust
                        bandit, three starving bandits, two dust bandits, a
                        dust bandit boss, and the boss with a second. Dust
                        bandit squads keep only the plain roaming AI package
                        (the vanilla one also raids towns).

The plugin refers to all of these by their stringIds.

    python -I scripts/make_mod.py <Kenshi\\mods\\KenshiRogue>
"""

import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kmod  # noqa: E402

DEPENDENCIES = ["gamedata.base", "Newwworld.mod", "rebirth.mod", "Dialogue.mod"]
STARVING_MOB = ("1307-gamedata.base", "Starving bandit mob")
HUNGRY_BANDIT = ("1304-gamedata.base", "Hungry bandit")
HUNGRY_LEADER = ("1303-gamedata.base", "Hungry bandit leader")
DUST_SQUAD = ("1302-gamedata.base", "Dust Bandit squad")
DUST_BANDIT = ("510-gamedata.base", "Dust bandit")
DUST_BOSS = ("1301-gamedata.base", "Dust bandit boss")
ROAMING_AI = b"5201-gamedata.base"
TORCH_POST = ("5437-new_furniture_otto.mod", "Torch Post")
SIGNS_MATERIAL = ("973-gamedata.base", "signs")
THRONE_USE = ("57289-rebirth.mod", "throne chair")  # BuildingFunctionality, BF_THRONE
BOWING = ("44934-rebirth.mod", "bowing")             # the fogmen's worship animation

KENSHI_TOWN_ANKH = os.environ.get(
    "KENSHI_TOWN_ANKH", os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir,
                                     "kenshi-town", "build", "assets", "ankh"))
ANKH_FILES = ["ankh.mesh", "ankh.xml", "ankh.dds", "ankh_n.dds"]


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

    def new(base_rec, name):
        rec = kmod.clone(base_rec, f.new_id("KenshiRogue.mod"), name)
        rec.id = f.last_id
        return f.add(rec)

    # The ankh, as Kenshi Town's build_mods.py makes it (see there for why).
    where = ".\mods\KenshiRogue\\"
    mat = new(base(db, SIGNS_MATERIAL), "Kenshi Rogue ankh")
    mat.set("file", "texture map", where + "ankh.dds")
    mat.set("file", "normal map", where + "ankh_n.dds")
    mat.set("float", "specular mult", 0.3)
    torch = base(db, TORCH_POST)
    part = new(db[torch.refs[b"parts"][0][0]], "Kenshi Rogue ankh part")
    part.set("file", "phs or mesh", where + "ankh.mesh")
    part.set("file", "xml collision", where + "ankh.xml")
    part.refs[b"material"] = [(kmod._b(mat.sid), 0, 100, 0)]
    use = new(base(db, THRONE_USE), "Kenshi Rogue ankh worship")
    use.refs[b"animation"] = [(base(db, BOWING).sid, 0, 0, 0)]
    building = new(torch, "Kenshi Rogue: ankh")
    building.refs = kmod.OrderedDict([(b"parts", [(kmod._b(part.sid), 0, 100, 0)]),
                                      (b"functionality", [(kmod._b(use.sid), 0, 0, 0)])])
    building.set("str", "building category", "EXTERIOR")
    building.set("str", "Description", "Bow here to leave. Kenshi Rogue.")
    assert building.sid == kmod._b("6-KenshiRogue.mod"), building.sid

    # The encounter pool past fight 1: (base squad, leader, (member, count) or None, name).
    dust = base(db, DUST_SQUAD)
    pool = [
        (mob, base(db, HUNGRY_LEADER), None, "Kenshi Rogue: a hungry bandit leader"),
        (dust, base(db, DUST_BANDIT), None, "Kenshi Rogue: a dust bandit"),
        (mob, bandit, (bandit, 2), "Kenshi Rogue: three starving bandits"),
        (dust, base(db, DUST_BANDIT), (base(db, DUST_BANDIT), 1), "Kenshi Rogue: two dust bandits"),
        (dust, base(db, DUST_BOSS), None, "Kenshi Rogue: a dust bandit boss"),
        (dust, base(db, DUST_BOSS), (base(db, DUST_BANDIT), 1), "Kenshi Rogue: a dust bandit boss and his second"),
    ]
    for number, (squad_base, leader, members, name) in enumerate(pool, 7):
        rec = new(squad_base, name)
        rec.refs[b"leader"] = [(leader.sid, 1, 0, 0)]
        if members:
            rec.refs[b"squad"] = [(members[0].sid, members[1], members[1], 0)]
        else:
            rec.refs.pop(b"squad", None)
        rec.refs[b"AI packages"] = [(ROAMING_AI, 0, 0, 0)]
        # No talking first: dust bandits open with a "hand over your stuff"
        # conversation that waits on the player; a fight stalled 600 s on it.
        rec.refs.pop(b"dialog leader", None)
        rec.refs.pop(b"dialog squad", None)
        assert rec.sid == kmod._b("%d-KenshiRogue.mod" % number), rec.sid

    for name in ANKH_FILES:
        src = os.path.join(KENSHI_TOWN_ANKH, name)
        if not os.path.isfile(src):
            raise SystemExit("%s missing: build Kenshi Town's ankh (scripts\assets\build_ankh.cmd there)" % src)
        shutil.copy2(src, os.path.join(sys.argv[1], name))

    path = os.path.join(sys.argv[1], "KenshiRogue.mod")
    kmod.write(f, path)
    assert kmod.dumps(kmod.read(path)) == kmod.dumps(f), path
    print("wrote %s (%d records)" % (path, len(f.records)))


if __name__ == "__main__":
    main()
