# WotM glossary

Quick reference so everyone in the project uses the same names. Sources: `include/game/MonsterNames.h` (`MonsterNewNames`), `docs/FORMATOS.md` (`MonsterLongNames`) and the contents of `disc/MON/` (not versioned). What came from the project owner is marked as such.

## Monsters

The **index** is the game's (`MonsterLongNames`/`MonsterNewNames`). The **internal name** is the prefix of the files `disc/MON/<PREFIX><costume>.NGP/.TEX/.RTX/.PTR`; the number at the end is the **costume** (0 to 3), not the monster (`LIZARD1` is Togera in another costume).

| # | In-game name | Internal name | Files on disc | Notes |
|---|---|---|---|---|
| 1 | Congar | `MONKEY` | MONKEY0–3 | |
| 2 | Robo-47 | `ROBOT` | ROBOT0–3 | |
| 3 | Togera | `LIZARD` | LIZARD0–3 | |
| 4 | Ultra V | `SHOGUN` | SHOGUN0–3 | |
| 5 | Preytor | `MANTIS` | MANTIS0–3 | |
| 6 | Spider | `SPIDER` | none | cut; leftovers below |
| 7 | Kineticlops | `ENERGY` | ENERGY0–3 | |
| 8 | Magmo | `LAVA` | LAVA0–3 | |
| 9 | Raptros | `DRAGON` | DRAGON0–3 | |
| 10 | Agamo | `ROCK` | ROCK0–3 | |
| 11 | Zorgulon | `ALIEN` | ALIEN0–3 | |
| 12 | Ogre | `OGRE` | none | cut; leftovers below |
| 13 | Goliath Prime | `ASSBOSS` | ASSBOSS0–1 | name given by the owner; the code calls it `assboss` |
| 14 | Final alien | `JELLY` | JELLY0 | final boss, 2nd phase: the jellyfish-like robot that comes out of the big robot (`FinalBoss::updateJelly`) (the first phase does not count as a monster; the stage loads it instead) |
| 15 | Final alien | `FINAL` | FINAL0 | final boss, 3rd phase: the alien in raw form, attacks with claws |
| 16 | — | `temp16` | — | name only; the loader rejects index >= 16 |

There are 17 names (0 to 16): `MonsterNewNames` is 0xCC bytes (17 × 12) and `MonsterLongNames` 0x88 (17 × 8). The `default` that appears right before it in the binary is `GameLevelNames[0]` (stage 0), not a monster.

Names are lowercase in the code (`"robo-47"`, `"ultra-v"`); the capitalization in the "In-game name" column is ours.

### Cut monsters and special slots (survey from 2026-10-14)
Everything here was read from the ELF (strings, stabs, symbols) and from the disc; what is deduction is marked.

- **Neither of the two has files.** There is no `SPIDER*`/`OGRE*` in `MON/`, no `.AIC`, no sound bank (`loadMonsterBank` only knows 10 banks, `Ape_s.bnk`..`Alien_s.bnk`; slots 5 and 11 of `gSoundBanks` stay empty), and no intro music VPK.
- **Outside monster select.** `monsterNametomonsterId` (0x2A1A40, column order) is `dragon, lizard, mantis, monkey, robot, rock, shogun, lava, energy, alien, spider, ogre, assboss, jelly, final`; the screen only walks the first 10 columns (`initSelectSwitches`: `[first, last)` with `last` <= 10). Spider and ogre sit right after Zorgulon, ready to become columns.
- **Excluded on purpose.** Demo mode (`EvaluateGameStatus`, `m_mode` 0) draws `mathfRand(1, 12)` and swaps 0xC0 (spider) and 0x180 (ogre) for Congar; mode 6 loads 1..5 and 7..11 (skips the spider; the ogre is outside the range); `aiMonsters[10]` in `Shell.cpp` does not have the two either.
- **Ogre code leftovers:**
  - `MonsterSound::updateMonsterSound` still dispatches 0x180 to `updateOgreSound`, which is empty.
  - `Monster::updatePowerUpGlow` has its own `case 0x180` (pulse with minimum 0x20 and base 0x80, different from the others); the spider (0xC0) shares the Kineticlops (0xE0) case.
  - The `OgreMace` class (TU `game/OgreMace`: ball and chain, level ep 0x11FC in `dbProcInteractive`) is used by `StateGrappleHook` at `Monster+0x10600`, which `initDynamics` attaches to **Ultra V** (0x80). Deduction: the ball and chain was written for the Ogre and reused as Ultra V's hook.
- **Leftovers in the effects enum (`FxId`, stabs):** `SpiderHit` (91), `SpiderSpecialHit` (103), `SpiderFlashBang` (114), `OgreSpecialHit` (99), and also `WomanSpecialHit` (98), `ErnieHit` (87) and `ErnieSpecialHit` (100), from monsters that do not even appear in the name list (other cuts or old names; not identified). No call to `particleCreateFx` with these ids was found; `JellyfishSplash` (86) is used in `updateJelly`.
- **Spider in the sound code:** `MonsterSound::updateSpiderMagnoSpecial`/`terminateSpiderMagnoSpecial` exist but nobody calls them, and the first one never fires the sound (it only adjusts a handle that is never created). It suggests the spider's special was something called "magno".
- **Final boss (`capitol`).** Phases, according to the owner: (1) a big robot that fires a high-damage laser, and the player hides behind the pillars or other geometry; (2) after enough damage, a robot resembling a jellyfish comes out of it (it does not stay in the water); (3) the alien in raw form, attacking with claws; (4) once it is defeated, a cutscene shows the brain exploding and a fourth, small creature coming out of the head. In the code:
  - The phase lives in `finalBoss+0x1C`: 0 = big robot, 1 = jelly, 2 = final. `Shell::InitPlayers` restores the phase's health on restart (in phase 0 it rebuilds the pillars with `rebuildCapitolPillars`).
  - The big robot is the `FinalBoss` object itself (`FinalBoss::updateMecha`: laser `FinalBossSound::updateMechLaser`, shield `playShieldPowerUp/DownSound`, `playMechaPowerDownSound`, `mechaShadowRadius`, and a line-of-sight test `hdLosCSTest`, which must be what makes the pillars cover). It is not a monster from the list: the model is in `CAPITOL.NGP` (checked with `tools/ngp.py`). `FinalBoss::addEpNode` stores eps 0x27A6..0x27A9, 0x27AB and 0x27AD, and `initAfter` binds the boss's `_cs` to ep 0x27A6 (`finalBoss+0x364`, `hierSetCsEpNode`). In the stage, 0x27A6 is a CHAR_INSTANCE with 9 objects and ~8,500 vertices, 43 SKEL_BONE, 52 ANIM_XFORM and 16 ATTACHPT, with a local extent of ~810 × 285 × 400: it is the big robot. The others (0x27A7: 2 objects, ~283 in diameter; 0x27A8: a flat disc of ~146; 0x27A9: 4 small pieces) are accessories not yet identified. The 6 pillars are the destructibles 0x27AE..0x27B3 in the same id range.
  - `jelly` and `final` are the AIs of `capitol` in `levelMonsters`. `TheGame::SetAIMonster` counts both as AI, turns them off with `disableMonster` and stores them in `finalBoss+0x74` (jelly) and `+0x78` (final) until their phase. `FinalBoss::updateJelly` handles the jelly: `StateUltraTazer`, `electricArc`, `playJellyTazerOneShot`. It also calls `playWaterSplashSound` and creates the `JellyfishSplash` effect; since the jelly does not fight in the water, these two must belong to some specific moment (to be checked).
  - Sound: `Final_s.bnk`, `JELLYD.VPK`, `JELLYBT.VPK`, `MECHADT.VPK`. The AIs have `CAPITOL/JELLY0–2.AIC` and `FINAL0–2.AIC`.
- **Old monster videos.** `MOV/` has a simple 32 KB `.PSS` per monster, from Nov/2001 to Jan/2002, including `OGRE.PSS` (an exact copy of `SHOGUN.PSS`) and `SPIDER.PSS` (an exact copy of `ENERGY.PSS`). The executable references none of them; it only uses `mov/<monster>_p.pss` and `_g.pss`, and only for the 10 playable monsters. It shows the ogre and the spider were still in the cast in early 2002.
- **Goliath Prime (`ASSBOSS`, `canyon2`).** The fight, according to a fan wiki the owner brought: first the Mutant Ants; then Goliath throws time bombs (green, yellow, red) from the top of the bunker, which the player throws back until it has been hit 8 times; on the ground, it grabs and charges with its shoulder propellers, which hits the pillars; in the last part, it alternates a spin in a "T" pose with volleys of homing missiles. A big rock in the stage interrupts the attacks. In the code:
  - The Mutant Ants are Preytor in costume 1: `levelMonsters[3]` = 3 × `mantis` + `assboss`, all with `levelMonsterModels` 1 (`MANTIS1`), and `CANYON2/MANTIS0–2.AIC`.
  - `canyon2LevelProgression` (0, 3, 6) and `Ai::updateAssBossPatternZero/One/Two` match the three parts; `assBossTier1Health`/`assBossTier2Health` are the health thresholds between them, and `Shell::InitPlayers` restores health and rebuilds the pillars (`rebuildCanyon2Pillars`) on restart.
  - The propellers and the spin show up in the sound code: `MonsterSound::updateAssBossRotorSound`, `m_assBossSpinHandle`.
  - The two costumes: `ASSBOSS0` (Mar/2002, 91 KB of texture) is the old silver version and `ASSBOSS1` (Sep/2002, 836 KB) the final one. With `MonsterLongNames`, the loader accepts index 13 as a player (it is less than 16), which explains why it can be played with a GameShark code.
  - The name `assboss`: the sound bank loaded by `SoundManager::loadMechBossSoundBank` is `"Assemb_s.bnk"` (`D_006EFFB0`), which suggests "assembly boss"; nothing in the code confirms it, and the wiki does not mention assembly either. It is still unknown who sets `m_loadMechBank`.
- **The plant boss (`3-Mile`)** is not a monster from the list: it is the classes `PlantBoss`/`PlantBossHead`/`PlantBossRoot`/`PlantBossBase` (`plantBoss`, 0x10B0 bytes), with `Plant_s.bnk` and `PLANTD.VPK`; the stage's AIs are 4 Kineticlops (`3-MILE/ENERGY0–2.AIC`).
- **temp16:** exists only in the two name tables. `Shell::LoadResTexture` rejects `sel >> 5 >= 0x10` with "Unknown monster (%d) for player %d.  Nothing will be loaded!".

### Open questions
- The names the game shows for the jelly and the final.
- Where the final cutscene is (brain exploding, small creature): there is no boss `.PSS` in `MOV/`, so it must be a real-time cinematic.
- Who `Woman` and `Ernie` from `FxId` are.
- The spider/ogre could be put on the select screen (widen `last` in `initSelectSwitches`), but without `MON/SPIDER0.*`/`OGRE0.*` `fileReadf` finds no file; it would need a substitute model.

## Stages

The **index** is the game's (`GameLevelNames`, 0x6EF748: 28 names of 10 bytes) and is what `Shell::m_levelNum` holds. The **internal name** is that of the files `disc/LVL/<NAME>.NGP/.TEX/.RTX/.PTR`. The playable stages also have `MOV/<NAME>.PSS`, `PTS/<NAME>.AIP/.FOP/.PED/.PUP/.SPT` and `RST/<NAME>.MRS`. `disc/LEVELS.TXT` lists the 15 that have a `.ngp`. The **in-game name** is what the player sees, taken from `GameLevelLongNames` (0x6ED120, same index; confirmed by the owner).

| # | In-game name | Internal name | Files on disc | Notes |
|---|---|---|---|---|
| 0 | Default | `default` | — | command-line stage (`whichLevel`, `UseCommandLineLevel`) |
| 1 | Midtown Park | `central` | `CENTRAL` | the stage the `-Auto` script reaches |
| 2 | Gamblers Gulch | `vegas` | `VEGAS` | |
| 3 | Rosdale Canyon | `canyon2` | `CANYON2` | in story mode, the fight against Goliath Prime (`assBoss`, `canyon2LevelProgression` in `Shell::InitPlayers`) |
| 4 | Metro City | `metro` | `METRO` | |
| 5 | Century Airfield | `airport` | `AIRPORT` | |
| 6 | Atomic Island | `3-Mile` | `3-MILE` | in story mode, the fight against the plant boss (`plantBoss`, `craterSwitched`, `threeMileInitAi`) |
| 7 | Baytown | `sanfran` | `SANFRAN` | |
| 8 | Club Caldera | `island` | `ISLAND` | |
| 9 | Tsunopolis | `tokyo` | `TOKYO` | |
| 10 | UFO | `ufo` | `UFO` | |
| 11 | Capitol | `capitol` | `CAPITOL` | in story mode, the fight against the final boss (`finalBoss`: big robot, then the monsters `JELLY`/`FINAL`, 0x1C0/0x1E0) |
| 12 | Training | `training` | `TRAINING` | training |
| 13 | Lake Powell | `canyon1` | `CANYON1` | |
| 14 | Mini Baytown | `minisf` | `MINISF` | |
| 15 | Volcano | `volcano` | `VOLCANO` | |
| 16 | Tokyo Deathmatch | `tokyodm` | `TOKYODM` | |
| 17–24 | Dylan, David, Lars, Jenn, Yeon, Jacob, Steve, Pierre | `dylan`, `david`, `lars`, `jenn`, `yeon`, `jacob`, `steve`, `pierre` | none | people's names: probably the team's test stages, no files |
| 25 | Dodge Ball | `dball` | `DBALL` | dodgeball minigame (`m_mode` 7) |
| 26 | Big Shot | `bigshot` | `BIGSHOT` | bigshot minigame (`m_mode` 8) |
| 27 | Crush | `crush` | `CRUSH` | crush minigame (`m_mode` 9; the game text also says "Crush-o-Rama") |

The path through `Shell::LoadResTexture` shows that indices above 0x1C fall back to `host0:monster.rtx`.

## Other terms
(to be filled in)
