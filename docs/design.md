# Kenshi Rogue: design

Started 2026-11-23. A plan, plus what the first runs proved. The first slice works in game (2026-11-23).

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

## Bill's direction (2026-11-23, after the first slice ran)

- **A run opens with a choice of 3 boons**, Slay the Spire style, drawn at random from a pool:
  e.g. +10 Attack, +10 Sabres, or a better random weapon (a Mid-Grade Hacker) instead of the
  stock loadout.
- **When a fight ends, an ankh appears** (the Faience Ankh from Kenshi Town's asset pipeline,
  as a placeholder). You loot, bandage, finish people off, whatever you like. Then you
  **right-click the ankh to bow** (1–2 s at most), and that takes you back to the **node map** to
  choose the next engagement. This replaces the HUD's "next" button.
- **No milking the time between fights** for free healing. Decided (Bill agreed to Claude's
  proposal, 2026-11-23):
  - **natural healing is off** for runners during a run (hook `HealthPartStatus::update`'s heal
    multipliers). First aid stops bleeding and splints, but only rest nodes restore flesh;
  - **a generous soft timer** after each fight, shown on the HUD, after which the ankh calls you on.
- **Money after every engagement**: a random amount of cats, so it can be spent.
- **Shops are real towns** (Stoat, Flats Lagoon, …): a shop node takes the squad to one, where
  they browse Kenshi's own traders and spend the run's money. They get a more generous timer
  there (around 10 minutes). **They shouldn't be able to leave the town.** An ankh in the centre
  or by the entrance takes them back to the node map.
  - Leaving the town, proposed (Claude, not yet decided): a leash. Crossing the town's edge
    teleports the squad back to the ankh with a message, and when the timer runs out the ankh
    takes them on.

## What the first runs taught (2026-11-23, against `kr-test`)

- **Enemies need telling.** A squad spawned without a target stood 25 m away for minutes. Now the
  runner is the squad's AI target and each enemy gets `attackTarget(runner)`, repeated every 10 s.
- **Ground decides everything.** 200 m from a chosen spot put the runner on a mesa nobody could
  path to, and 25 m put enemies up a cliff. The arena is where your character stood. Your
  characters are teleported 200 m aside for the run, and enemies spawn 6 m away.
- **Your own characters will join in** if they're near (a skeleton tanked fight 1).
- **The world walks in.** A Dust Bandit squad of 11 sat on the test spot, and starving bandits
  passed through. Everyone not in the run is removed within 150 m for the whole run. A hook on
  the spawner would be the thorough version.
- **Knocked out isn't over.** A downed bandit got up and joined the next fight, so a fight's
  enemies leave when you move on (loot first).
- **Weapons are made differently from items** (KenshiCoop's finding): maker first, a `NULL_ITEM`
  hand, then template, grade, level 0.
- **`fleshStun` is stun damage.** Rest sets it to 0. Setting it to the maximum knocks the
  character out.
- **A heavy starting weapon costs Attack:** a Horse Chopper at Strength 10 showed Attack 5 (−5).

## First win rates (trials, 2026-11-23)

A fresh runner each bout: Strength, Dexterity, Toughness, Athletics and every melee skill at 10,
Dodge 5, and a Rusted Junk weapon. Six bouts each, so these are rough.

| weapon | fight 1 (one starving bandit) | fight 2 (two) |
|---|---|---|
| Horse Chopper | 5 / 6 | 1 / 6 |
| Ringed Sabre | 2 / 4 (two bouts void: no enemies found, since fixed) | 0 / 6 |
| Katana | 6 / 6 | 1 / 6 |

Fight 1 is about right, the gentle opener it's meant to be. **Fight 2 is a cliff**: two starving
bandits beat a fresh runner five times in six. Between the two fights, a run needs a boon, a
better weapon, or a step that's smaller than doubling the enemies.

## Open questions

- ~~Does RE_Kenshi load a plugin only when its mod is enabled?~~ **Yes** (checked 2026-11-23):
  with KenshiRogue installed but not in `data/mods.cfg`, only TownLedger's plugin loaded, and
  KenshiCoop, installed and disabled, didn't load either. Listed in `mods.cfg`, KenshiRogue loaded
  beside TownLedger. So a harness run picks its mods by writing `mods.cfg`, then puts the
  player's list back.
- Where the arenas are, beyond "where you stand". Flat ground matters more than anything.
- The runner's sheet: every melee skill at 10 for now, so trials compare weapons.
- Does fight AI behave in an empty arena (fleeing, kiting, getting stuck)?
- Later: runs as expeditions from a Kenshi Town settlement. Keep the two standalone until both
  work; then shared plugin code moves to a common library.
