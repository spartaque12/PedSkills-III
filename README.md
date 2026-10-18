# PedSkills III

A GTA III port of [PedSkills](https://github.com/JuniorDjjr/PedSkills) by JuniorDjjr, originally for GTA San Andreas.

It controls how well NPCs shoot. Each ped type gets its own accuracy and armour. Cops miss more when you move across their view, when you're far away, or when they fire from a vehicle. They can also fight from farther away instead of running up to you. Everything is set in `PedSkills.III.ini`.

Only GTA III 1.0 is supported.

## Install

You need an ASI loader, such as the one included with [SilentPatch](https://github.com/CookiePLMonster/SilentPatch) or [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).

1. Copy `PedSkills.III.asi` and `PedSkills.III.ini` into the game's `scripts` folder.
2. Edit the ini. Every option is explained in it.

## How GTA III aims

The ini's numbers follow how the game itself shoots, as found in `CWeapon::FireInstantHit` (the game at `0x55D2E0`, and re3):

- **Accuracy runs from 0 to 100.** When a ped fires at its target, the game aims at that target and pushes the end of the shot, at the weapon's full range, off by a random ±0.2 × (100 − accuracy) metres sideways (±0.1 vertically). So 100 never misses.
- **Values above 100 aren't better.** The game doesn't clamp them: 120 misses like 80, and 255 is the worst possible value. PedSkills clamps everything to 0–100.
- **A ped whose arm hasn't locked onto the target fires straight out of its hand** and ignores accuracy. With `AlwaysAimAtTarget = 1`, NPCs facing their target aim at it anyway.
- **Peds start shooting once you're within the weapon's range.** When you're farther than that, they first run in to a third of it: for the 30 m pistol, that's 10 m. `WeaponRangeMult` makes them fight from farther away. NPC accuracy is adjusted to match, so a longer range doesn't also make them more accurate.

The game's own accuracy values are: cops 60, SWAT 68, FBI 76, army 84, every other ped 60, the player 100.

## What it changes in the game

- **Each shot:** a hook where `FireInstantHit` reads the shooter's accuracy (`0x55D55F`) replaces it with the accuracy for that shot. This is the same approach as the SA mod.
- **Aiming:** with `AlwaysAimAtTarget`, a hook on the arm IK check (`0x55D550`) lets NPCs facing their target aim at it.
- **Spawn values:** the accuracy and armour each ped type spawns with are patched in `CCopPed::CCopPed` and `CPed::CPed`.
- **Missions:** a hook on opcode `02E2 SET_CHAR_ACCURACY` (`0x446CA5`) applies `ScriptAccuracyMult` to accuracy set by mission scripts.
- **Range:** `WeaponRangeMult` scales `m_fRange` in the loaded weapon data.

The hooks are plain jumps to small assembly stubs. They're installed when the game starts its scripts, after every other ASI has loaded. An earlier build installed them while the ASIs were still loading, and that made `vehicle_audio_loader_for_1.0.asi` crash at startup.

## Build

You need Visual Studio 2022 or its Build Tools with the C++ x86 tools, plus a built [plugin-sdk](https://github.com/DK22Pac/plugin-sdk) (`Plugin_III.lib`).

1. Set `PLUGIN_SDK_DIR` to the plugin-sdk folder. Without it, the project looks in `..\..\plugin-sdk`.
2. Build `Release GTA3 | Win32`:

```bash
msbuild PedSkills.vcxproj /p:Configuration="Release GTA3" /p:Platform=Win32
```

The output is `bin\GTA3\Release\PedSkills.III.asi`. If `GTA_III_DIR` is set, the build also copies it to the game's `scripts` folder.

## Credits

- [JuniorDjjr](https://github.com/JuniorDjjr): the original PedSkills for GTA SA.
- The [re3](https://github.com/Hezkore/hez-gta-re3) project: the reference for how GTA III aims.
- [plugin-sdk](https://github.com/DK22Pac/plugin-sdk).
