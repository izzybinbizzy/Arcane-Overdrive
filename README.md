# Arcane Overdrive - SKSE plugin

Copyright (C) 2026 izzydoingit. GPL-3.0-or-later, see `LICENSE`.

The plugin of Arcane Overdrive - it is the whole mod: no plugin file, no scripts, nothing written into the save.

## What it does
- Casting builds a heat meter through four stages: Warm, Hot, Overheating and Overdrive. Each stage makes damage spells a
  little stronger and the lights on the casting hands a little brighter and wider.
- Staying in one school builds heat faster, switching schools costs a stage, and heat drains a moment after the last cast.
- Overdrive (automatic, on a key, or off): a short burst at the top bonus where the spells in hand grow, then a cooldown
  with slower magicka.
- From Overheating up, fire and frost spells leave burning or frozen patches where they land (they can hurt, a switch).
- The heat on the HUD: a live bar drawn from the spell's own art, or a bar beside Reticle Arcs' crosshair when it is
  installed.

## Requires
SKSE and Address Library for SKSE Plugins; SKSE Menu Framework for the menu and the HUD bar. Reticle Arcs is optional.

## Settings
SKSE Menu Framework > Arcane Overdrive, saved to `Data\SKSE\Plugins\SpellMasteryHeat.ini`. The shipped settings file and
the HUD bar's art are in `package/` (installed to `Data\SKSE\Plugins\`).

## Building
`build.bat` (git, xmake 3 and the Visual Studio C++ tools): CommonLib at the pinned commit, the DLL, then the off-game
tests (`tests/test_heat.cpp`).

## Third-party files
`src/SKSEMenuFramework.h` is SKSE Menu Framework's own header by Thiago Kaique, copied unchanged from
[SKSE-Menu-Framework-3-Example](https://github.com/QTR-Modding/SKSE-Menu-Framework-3-Example), MIT licensed
(see `src/SKSEMenuFramework.LICENSE.txt`).

`src/DevBenchAPI.h` and `src/DevBenchAPI.cpp` are devbench's own interface files, MIT licensed by
their author (see `src/DevBenchAPI.LICENSE.txt`) and copied unchanged. devbench is not required.

`src/ReticleArcsAPI.h` is Reticle Arcs' add-on interface by Chiktabba, MIT licensed (the notice at its top), copied
unchanged. Reticle Arcs is not required.
