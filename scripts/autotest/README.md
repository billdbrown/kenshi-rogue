# Autotest scenarios

    python -I scripts/autotest.py scripts/autotest/<scenario>.txt [--no-install] [--timeout N]

Carried over from kenshi-town's runner. It builds and installs, writes the scenario to
`Kenshi/mods/KenshiRogue/autotest/scenario.txt`, launches Kenshi, waits for the plugin's
report, ends Kenshi and removes the scenario. Results and screenshots land in
`build/autotest/<scenario>-<time>/`. It takes over the screen, and refuses to start if Kenshi is
open.

**Mods.** RE_Kenshi loads a plugin only from a mod enabled in `data/mods.cfg` (checked
2026-11-23). For the run, the runner writes the player's list with Kenshi Town's mods
(`TownLedger.mod`, `KenshiTownDev.mod`) taken out and `KenshiRogue.mod` added. It puts the
player's list back afterwards, byte for byte.

Nothing is saved during a run, and autosaves are held off.

## Commands (one per line, `#` comments)

| command | does |
|---|---|
| `load <save>` | from the main menu, loads a save |
| `wait_ready [secs]` | waits until in game and settled 5 s (default 240 s) |
| `wait <secs>` | real seconds |
| `speed <x>` | game speed |
| `screenshot <name>` | `<name>.png` of the game window |
| `note <text>` | a line in the report |
| `run_start [east m] [south m]` | starts a run: the runner is created that far from your first character (default 300 m north), and fight 1 begins |
| `wait_fight [secs]` | until the current fight is won or lost (default 300 s); fails if still undecided |
| `next` | what the HUD button does between nodes: rest, or the next fight |
| `loot` | everything the run's enemies carry goes to the runner's pack (looting by hand, done for the test) |
| `runner` | the runner's state, blood, every body part, lost limbs, and pack |
| `enemies` | each enemy: state, body parts, what they carry |
| `wound <amount>` | takes that much off every body part of the runner (to test rest) |
| `focus` | camera on the runner |
| `strangers` | how many outsiders the arena removed during fights |
| `expect state <none\|fighting\|between\|won\|lost>` | the run's state |
| `expect fights_won <n>` / `expect enemies <n>` / `expect runner_up` | counts and state |
| `expect carrying <item name>` | the runner's pack has it |
| `quit` | writes DONE and ends Kenshi (also at the end of the file) |

## Report

`report.txt`, one line each: `STEP`, `INFO`, `PASS`, `FAIL`, `SHOT`, `NOTE`, ending
`DONE pass=N fail=M`. The runner exits 0 only with a DONE and no FAIL.
