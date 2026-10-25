# Arcane Overdrive - SKSE plugin

Copyright (C) 2026 izzydoingit. GPL-3.0-or-later, see `LICENSE`.

The plugin of Arcane Overdrive - it is the whole mod: no plugin file, no scripts, nothing written into the save.

## What it does
- Two bars. Casting fills the Overdrive bar (faster in one school, a school switch costs some, it drains a moment after
  the last cast). Casting fast builds Heat, which cools when you ease off; its stages - Warm, Hot, Overheating - make
  damage spells a little stronger and the lights on the casting hands a little brighter and wider.
- A full Overdrive bar goes when Heat lets it: by default only in a sweet spot (too cold does nothing, too hot misfires);
  presets Milk Drinker, Dragonborn and Aedra. Overdrive is a short burst at the top bonus, then a burnout with slower
  magicka.
- The spells grow with the Overdrive bar (the casting art, projectiles, the player's explosions and hazards), turn wild
  while Heat is in the red, and are drawn in and steadied as Overdrive fires (`src/Overload.*`).
- From Overheating up, fire and frost spells leave burning or frozen patches where they land (they can hurt, a switch).
- The Overdrive bar is TrueHUD's special bar, colored by the Heat (`src/TrueHud.cpp`, through `src/TrueHUDAPI.h`);
  both bars can also be Reticle Arcs arcs round the crosshair (`src/Arcs.cpp`).
- Clashes: projectiles that meet in the air clash by element (the stronger flies on, or both go and burst); spell
  struggles: sprays, held beams and breath that meet head on lock and push, the higher magic skill breaks through
  (`src/Clash*`, the game-free core tested by `tests/test_clash.cpp`); auto lock aims bolts at the foe and
  steers them into oncoming spells (`src/ClashAim.cpp` - the way a projectile is turned is True Directional Movement's).

## For other mods
Papyrus gets the mod events `ArcaneOverdrive_Clash` (strArg "Fire,Frost", numArg 1 / 2 / 3 = the first / second /
both destroyed, sender the first's shooter) and `ArcaneOverdrive_Struggle` (strArg "began|overwhelmed|gave way|draw|ended,
<element>,<element>", numArg 1 the player won, -1 lost); SKSE plugins listen with `RegisterListener("SpellMasteryHeat", ...)`
- `src/CrossfireAPI.h`.

## Requires
SKSE and Address Library for SKSE Plugins; SKSE Menu Framework for the menu. TrueHUD (the bar) and Reticle Arcs (the
arcs) are optional.

## Settings
SKSE Menu Framework > Arcane Overdrive (every part on or off, the preset), saved to
`Data\SKSE\Plugins\SpellMasteryHeat.ini`; every number in `SpellMasteryHeat Advanced.ini` beside it, each line explained.
The clashes: `SpellMasteryHeat Clashes.ini` (the menu's) and `SpellMasteryHeat Clash Rules.ini` (what meets what; patches
in the `SpellMasteryHeat Clash` folder).
The shipped settings files are in `package/` (installed to `Data\SKSE\Plugins\`).

## Building
`build.bat` (git, xmake 3 and the Visual Studio C++ tools): CommonLib at the pinned commit, the DLL, then the off-game
tests (`tests/test_heat.cpp`, `tests/test_clash.cpp`).

## Third-party files
`src/SKSEMenuFramework.h` is SKSE Menu Framework's own header by Thiago Kaique, copied unchanged from
[SKSE-Menu-Framework-3-Example](https://github.com/QTR-Modding/SKSE-Menu-Framework-3-Example), MIT licensed
(see `src/SKSEMenuFramework.LICENSE.txt`).

`src/DevBenchAPI.h` and `src/DevBenchAPI.cpp` are devbench's own interface files, MIT licensed by
their author (see `src/DevBenchAPI.LICENSE.txt`) and copied unchanged. devbench is not required.

`src/ReticleArcsAPI.h` is Reticle Arcs' add-on interface by Chiktabba, MIT licensed (the notice at its top), copied
unchanged. Reticle Arcs is not required.

`src/TrueDirectionalMovementAPI.h` is True Directional Movement's interface by Ershin
([ersh1/TrueDirectionalMovement](https://github.com/ersh1/TrueDirectionalMovement)), GPL-3.0, copied unchanged.
True Directional Movement is not required.
