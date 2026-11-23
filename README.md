# kenshi-rogue

A roguelite game mode for Kenshi, built as a native plugin on
[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) and KenshiLib.

Kenshi's fighting is good, but learning it means getting beaten up at random until your stats
catch up. Kenshi Rogue puts that curve in order: a run is a path of encounters that starts with
one starving bandit and grows with your squad, with loot drawn from Kenshi's own weapon and armour
grades, rest stops, and recruits along the way. Limbs you lose stay lost for the run.

## Status

Scaffold. The plugin loads and hooks the main loop; nothing playable yet. The plan is in
[docs/design.md](docs/design.md).

## Requirements

- Kenshi on Steam, 1.0.68. GOG is not supported by RE_Kenshi.
- RE_Kenshi 0.3.4, which ships KenshiLib 0.4.0.

## Building

KenshiLib plugins must be compiled with the **VC++ 2010 (v100) x64** compiler. You do not need
full Visual Studio 2010:

1. Any Visual Studio 2019 or newer, or the 2022 Build Tools, for MSBuild.
2. The Windows SDK 7.1 ISO and the VC++ 2010 SP1 compiler update (KB2519277).
   `scripts/install-v100-toolchain.ps1` installs the compiler MSIs straight from the ISO.
3. Dependencies:

       git clone https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps third_party/KenshiLib_deps
       tar -xf third_party/KenshiLib_deps/boost_1_60_0/boost.zip --directory third_party/KenshiLib_deps/boost_1_60_0

Then:

    scripts\build.cmd            build to build\KenshiRogue.dll
    scripts\build.cmd install    build and copy mod\KenshiRogue into Kenshi\mods

Kenshi must be closed to install; it locks the DLL.

## Licence

GPL-3.0, as KenshiLib and RE_Kenshi require. Build tooling shared with
[kenshi-town](https://github.com/billdbrown/kenshi-town).
