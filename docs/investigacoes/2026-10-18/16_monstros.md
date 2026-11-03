# 16 monsters in one fight: feasibility (2026-10-18)

**Verdict: it can be done, in the PC port, and 16 is the exact ceiling.** The engine was sized for 16 loaded monsters; what limits it to 4 is the
menu, the PS2's 32 MB of RAM and a few tables in the Shell and the loader. None of that requires touching VU1/GS or the fight loop. On a real
PS2 it doesn't fit (not enough RAM).

## What already supports 16 (it's in retail)
| Structure | Where | Size |
|---|---|---|
| `TheGame::m_slots` (the `Monster`s, stride 0x11190) | `game.h`, 0xB80 | 16 |
| `TheGame::m_slotInteractive` | 0x1203FC | 16 |
| `EnemyInfo::s_info` (pairwise table, rebuilt every frame) | `enemy_info.h` | `[16][15]` |
| `m_monsters` (pointers: players at 0..3, AIs from 4 on) | 0x120380 | the header says `[8]`, but there are 0x20 free bytes up to 0x1203C0: **16** fit. Endurance already writes `m_monsters[8]` (1 player + 5 AIs, `screen.cpp`, `m_numAIs = 5`) |
| `hdIgnored` (cs ignored in collision) | `hd.cpp` | 32 |
| `Interactives` | | 1024 (the fight uses 180..400) |
| `texInfo` (loaded textures) | 0x50F100 | 1000; stage ~310..330 + ~12..18 per monster → 16 monsters ≈ 620 |
| Sound selection (`SoundManager::setSoundVolumeInfo`, `[4]` per `m_index`) | | only writes for `m_playerNum == 1` (human player): AI doesn't overflow it |

The fight loops (`gameResolveCollisions`, `gameResolveLifeAndDeath` in modes 2/3/11, `getClosestMonster`, `GetMonsterFromName`,
camera via `m_numAIs`) already walk `m_numSlots` or `m_monsters[4 + i]`.

Also proven in retail: Endurance keeps 6 monsters loaded and the Shell's `m_mode == 6` loads all 10 monsters at once
(`LoadMonstersDB`, `LoadTexture`, `LoadResTexture`).

## What prevents it today (measured)
1. **Guest RAM (the main blocker).** Stage and monsters are loaded contiguously from 0xA00000 (`fileOnlyNgpFile` +
   `getNextNgpLoadAddr`). Measured in dumps (`build/dumps*/ram_*.bin`, `fileStatus` at 0x445390):
   - Central + 2 monsters: ends at 21.6 MB, ~5.6 MB free above → about 4 more monsters fit (~6, like Endurance).
   - Metro + 2 monsters: ends at 29.0 MB, ~1.4 MB free.
   - One monster costs ~1.2..1.6 MB (`.NGP` 1.0..1.4 MB, `.TEX` 0.11..0.19 MB, `.RTX` ~20 KB). 16 monsters ≈ 20..25 MB on top of the stage.
   - **Way out:** the PS2Recomp runtime is parametric in `PS2_RAM_SIZE` (`ps2_memory.h`, power of 2, 147 uses via the constant):
     raise it to 64 MB and load the monsters above 32 MB (hook in the loader). Pointers are 32-bit and the memory is flat,
     so the `dbs` relocation works the same. The native layer already uses `wotm::Ram{rdram, PS2_RAM_SIZE}`.
2. **`fileStatus` has 14 slots** (`FILE_MAX_SLOTS`; the next slot stores the end): at most 13 files = stage + 12 monsters.
   It needs a bigger native table, with a hook in the `file.cpp` accessors (all small and already decompiled; check whether anything
   uses it directly outside `file.cpp`).
3. **Shell:** `m_monsterSel[4 + j]` goes past the header's `[8]` but lands in `pad2958` (free up to 0x29E0, fits up to j = 11, to check);
   **`m_costume[2 + j]` runs over `m_mode` (0x2C10) from AI 10 on.** The AIs' costumes have to come from outside (the loader hook).
4. **Menu:** the select screens go up to 4 (2P/3P/4P). The cheap path is to scale the fight through configuration (a variable/ini read in a
   hook before `LoadLevelFiles`), without touching the menu.
5. **`LoadMonstersDB` text:** players 3/4 build the path with player 2's monster (a retail quirk); the new
   hook must not inherit that.

## Risks to measure (not blockers, may need adjustment)
- **Performance.** With 4 monsters the CPU sits at a 6.2 ms median (`NATIVO.md`). Per monster, the logic, the skinning and
  `EnemyInfo::update` grow (one `hdLosCSTest` per pair: from 1..6 today to up to 120..240 per frame). Rough estimate 12..20 ms: 60 fps
  at the limit; measure before optimizing.
- **Sound.** `loadRTSoundBanks` uses "lo-res" banks when there are more than 3 slots and loads one bank per monster type (up to ~10 distinct
  types) into SPU RAM (2 MB). Check whether the native sound layer (`PS2X_SND=1`) gets around the SPU limit; if not, sounds will be missing.
- **Resident VRAM (`.RTX`).** `maxResAddr` goes up 128 blocks (32 KB) per monster: 4352 → ~6400 with 16, below the 7298 where
  the `.TEX` area starts. It should fit; check what lives between the two.
- **Spawn points.** `StartPoints` has few points per type; with 16 the monsters may spawn on top of each other. `gameGetStartPoint`
  tries to avoid that, but extra points (a stage mod) or spreading them out in the hook may be needed.
- **Stages with extra monsters** (bosses 0x1C0/0x1E0 in the Capitol, 0x1A0) use up slots: there the ceiling is below 16.
- **Gameplay.** AI, camera and HUD were designed for 1..2 humans and up to 3 AIs. The free-for-all modes (2/3/11) already handle
  all slots; story and bosses won't work.
- **Going past 16** would require changing `sizeof(TheGame)` (the 16 `Monster`s take 1.1 MB in the middle of the object) and `EnemyInfo`: out of
  the question without decompiling and rewriting all of `TheGame`.

## Suggested plan (in steps, each testable)
1. **6..8 monsters without touching RAM:** hook in `Shell::LoadMonstersDB/LoadTexture/LoadResTexture/InitPlayers` reading the lineup
   from a variable (e.g. `WOTM_LINEUP`), free-for-all mode, light stage (Central). Proves `m_monsters[>8]`, `pad2958`, AI and camera.
2. **64 MB RAM** in the runtime fork + loading the monsters above 32 MB; regenerate `tools/runtime_patch/wotm-runtime.patch`. **Done on 2026-10-19** (`92af448:docs/passos/POS_FRONTEIRA.md`, item 1): Metro with 8 monsters runs.
3. **Native `fileStatus`** (32 slots) and AI costumes outside the Shell. From there, 16.
4. Measure frame time and sound with 16; optimize `EnemyInfo` if it's the culprit.

Estimated effort: step 1 in one or two days; 2 and 3 together in a few days; the rest depends on what the measurement shows.

## Result of step 1 (8 monsters, same day)
Done without touching RAM: `WOTM_MATCH` accepts up to 7 AIs (`native/runtime/wotm_overrides.cpp`) and `tools/lote.ps1 -AIs` goes up to 7.
The block at 0x1B70000 that looked occupied is leftover from the menu: the load area goes up to ~0x1F10000 (Metro with 2 monsters reaches
0x1D0C380). Central + 8 monsters ended at 0x1CC3400, with 2.3 MB to spare; a 150 s fight without hanging, all fighting, 60 fps,
~9 ms of CPU per frame. Details in `docs/passos/NATIVO.md` ("Up to 8 monsters").

### 8 monsters on every stage in the batch (30 s each, AI vs AI)
End of the load with 8 random monsters (headroom up to the 0x1F10000 ceiling): Central 2.6 MB, Vegas 2.1, Airport 7.5, San Francisco 1.8,
Island 2.9, Tokyo 5.2, UFO 4.6, Canyon1 8.8, MiniSF 4.6, Volcano 8.1, TokyoDM 5.9. All ran without hanging, crashing or slowing down.
**Metro (stage 4) doesn't fit:** the load went up to 0x23B4300 (35.7 MB), passed 32 MB and the game froze while loading (watchdog, `g_frame`
205). Only with the 64 MB RAM (step 2). San Francisco and Vegas are left with ~2 MB: heavier monsters (Zorgulon, Raptros, Kineticlops)
may hit the ceiling.
