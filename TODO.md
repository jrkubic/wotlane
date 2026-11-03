# TODO

What is left to do, and only that. Bug reports go first. A finished item leaves this file: the story goes into the commit message and what now exists goes into the reference doc (in the present tense). Doc rules: `CLAUDE.md`, "Documentation".

## Order
| # | Item | Depends on | Cost |
|---|---|---|---|
| 8 | Mods that add monsters (skins already exist) | — | medium/high |
| 10 | Playable bosses | 8 | high |
| 13 | Campaign (adventure) with up to 4 players | — | the highest |

10 is a special case of "add a monster" (8). The 4-player campaign comes last: cinematics, bosses and rules were made for 1 player.

### 8. Mods that add monsters
How it works today: `docs/passos/MODS.md` ("Skins", "Custom monsters").
- **Monsters with their own files:** a `files` field in `monster.json`, with a free type (6, 12 or 16) or remapping a type. Today a custom monster uses the type of one of the game's monsters. A new model depends on the `.NGP` coming back in through the kit (today it only goes out).
- **Skins in elimination (mode 6):** `Shell::LoadMonstersDB` loads costume 0 of the 10 monsters once, and the players switch monsters between rounds without reloading. Choosing a costume there requires changing the load (one file per chosen monster), not just the screen.
- **Mod file larger than the disc's** outside `MON/*`: the game loads into fixed-size regions and can overwrite what comes after (the log warns). Make room per file type.
- **Ceiling warning for a skin with its own `.TEX`/`.RTX`/`.PTR`:** a skin has no original to compare against, so today nothing warns. The load log should compare against the real ceilings: `texInfo[1000]` (final `maxTexId`), the `.RTX`'s resident VRAM and the `.PTR`'s 0x30D40 buffer (`0f091a4:docs/investigacoes/2026-10-23/skins_mod_carga.md`, item 8).


### 10. Playable bosses
- Where they stand: `MODS.md`, "Custom monsters" (Goliath, Jelly and Cerebulon; mod `mods/Cerebulon/`). The Plantboss is not viable.
- **Mecha** (Cerebulon's first phase): it is a piece of the final stage (`FinalBoss::updateMecha`, hier 0x27A6..0x27A9), not a `Monster`, like the Plantboss. Playable only as a new monster assembled from the pieces (`tools/monglb.py --boss mecha` already puts them together for the viewer). The owner's decision.
- Native selection: check the framing of the Jelly and the Cerebulon in the CUSTOM slot (`wotm_savepics.hpp`, `kFraming`).

### 13. Campaign with up to 4 players
- The campaign is for 1 player: the game mode (`m_gameMode`), the cinematics and the stages with bosses (extra slots 0x1C0/0x1E0 in the Capitol, 0x1A0) need to be revisited.

## Gaps (cheap, at any time)

## Ongoing
- **Off the critical path:** an oracle for functions with calls; coverage with gameplay to feed back into `boot_coverage_asm.csv` (`57d4afd:docs/investigacoes/2026-10-19/pendencias_oracle_cobertura.md`, sections 2 and 3).

## Out of order (until the owner chooses)
- **4 players always at 60:** with 4 views, ~80% of frames come out at 60 and the rest at 30; what holds it back is the EE (median ~14 ms, p90 ~17.5 ms). Candidates, from the profile of the EE samples (`PS2X_HOSTPROF=2` + `_RAW=1`, resolved with the `wotm.map` of the same build): the translated VU0 wrapper (`executeVU0Microprogram` copies ~1 KB of state each way, ~3%) and the out-of-line `checkpointDue` (~3%), both PS2Recomp seams; the VU1 packet assembly that `wotm.exe` discards (`pktAddVu1ObjAsm`, ~4%, risky: item G3 of `723b110:docs/investigacoes/2026-10-23/performance_vistas.md`). In the window, the particles still in rlgl's immediate mode (~0.6 ms per view). Compare by alternating within the same run (an AI vs AI fight does not repeat).



## Discarded by the owner (do not propose again)
- An unlock screen for mod skins (a mod skin is always unlocked) and a sound of its own per skin.
- 5 to 8 players (shared camera).
- A panel option to pin the LOD.
- Player frames of the native selection in 2D outside the big screen (they look better on the big screen).
- Entering training mode from the launcher (the menu's TRAINING line and `WOTM_TRAINING=1` are enough).
- Our own netcode (rollback): it would require proving that the logic always gives the same result from the same inputs.
- Making Goliath Prime or other bosses work in full (CPU, collision, animations that ASSBOSS does not have): custom monsters are mods/experimental.
