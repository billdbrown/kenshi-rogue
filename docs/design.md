# Kenshi Rogue: design

Started 2026-11-23. A plan, not a record of what works: nothing here is proven in game yet.

## The run

- A run starts with **one character with fixed stats** (no grinding before the run).
- The run is a **path of nodes**, shown in a plugin window: fight, elite, rest, trader,
  recruit, event. Pick the next node; the squad is taken there.
- **Fight 1 is always a 1v1 with a starving bandit.** After that, encounters are drawn from pools
  by depth.
- **Difficulty follows depth and the squad's actual combat stats**, so a run never jumps too far
  ahead of your people. Kenshi raises skills by fighting, which is the curve this mode puts in
  order.
- **The team grows**: recruit nodes, and sparing a downed enemy who then joins.
- **Limb loss carries through the run** (decided 2026-11-23). Losing everyone ends it.

## Fights

- Each fight happens in a **quiet arena**: the squad is teleported there and the encounter
  spawned at a distance (`createRandomSquad` with a chosen squad template, as Kenshi Town's
  `spawn_squad` does). The world's own spawns are switched off nearby.
- **Won** when every enemy is down or dead; **lost** when the whole squad is.
- Afterwards: loot the downed, finish them, or spare them.

## Rewards

Drawn from Kenshi's own grades, by depth:

- weapons: Rusted Junk → Mid-Grade Salvage → Refitted → Catun → Mk → Edge Types → Meitou;
- armour: Prototype → Shoddy → Standard → High → Specialist → Masterwork;
- consumables: bandages, splints, food.

## Between fights

- Rest nodes heal (a time skip or set limb health). Bandages carry you between them.
- Run state is kept per save, the way Kenshi Town keeps its per-game record.

## Testing

The Kenshi Town harness (`autotest.py`, a scenario file, a fixed test save) is the model. One
thing it adds here: **a fight plays itself**, since characters defend themselves. The harness can
run an encounter many times unattended and report a win rate, which is how encounters get tuned.

## First slice

1. A start that puts one fixed-stat character in an arena.
2. Fight 1 against one starving bandit; detect the win.
3. Loot: bandages and a Rusted Junk weapon.
4. A rest node.
5. Fight 2 against two bandits.

No node map yet. This proves the fight-end check and the teleport, with a harness scenario that
plays it unattended.

## Open questions

- ~~Does RE_Kenshi load a plugin only when its mod is enabled?~~ **Yes** (checked 2026-11-23):
  with KenshiRogue installed but not in `data/mods.cfg`, only TownLedger's plugin loaded, and
  KenshiCoop, installed and disabled, didn't load either. Listed in `mods.cfg`, KenshiRogue loaded
  beside TownLedger. So a harness run picks its mods by writing `mods.cfg`, then puts the
  player's list back.
- Where the arenas are.
- Does fight AI behave in an empty arena (fleeing, kiting, getting stuck)?
- Later: runs as expeditions from a Kenshi Town settlement. Keep the two standalone until both
  work; then shared plugin code moves to a common library.
