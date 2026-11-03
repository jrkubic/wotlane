# Gameplay modifiers: design

Date: 2026-11-02. Branch: wotlane/mac. Status: written for Lane's review (not approved yet).

## Goal

Three fun toggles for fights, playable online and offline, with the same results on every PC:

| Name | What the player sees | Strength (fixed) |
|---|---|---|
| `jump` (Super Jump) | moon gravity: jumps go about twice as high, thrown monsters and falling things float | game gravity x 0.45 |
| `fast` (Fast Monsters) | monsters walk, run, dash and ram 1.5x faster; animations and attacks normal speed | x 1.5 |
| `turbo` (Turbo) | the whole fight runs at 2x | 2 fields per frame, 1 vsync per frame |

Each one is on or off, at one tested strength. No sliders (YAGNI). They are chosen per fight and fixed for the whole fight.

## Section 1: how they flow (approved by Lane)

- **One value, three names.** Text form: comma list of the names on, in the canonical order `jump,fast,turbo`; empty = none.
  Unknown names are an error everywhere (server, runtime, tools).
- **Online:** the lobby owner sends `SET mods=jump,turbo` (owner only, like `stage`/`wins`). The server validates it, stores it
  in the lobby, includes `mods=` in every `SETTINGS` line (empty value when none) and in the `MATCH` line. `tools/net_lobby.py`
  (`match_env`) and the launcher turn it into `WOTM_MODIFIERS=<list>`, always set, empty when none.
- **Config hash:** `netConfigHash()` (`native/runtime/wotm_sync.inc`) includes the canonical modifier list. A PC with different
  modifiers is refused at join ("game configuration differs"), never desynced.
- **Offline:** a MODIFIERS tab in the in-game options panel (one on/off row per modifier) and three checkboxes in the launcher.
  Stored in `settings.ini` as `WOTM_MODIFIERS=jump,turbo`.
- **Network lock:** in a network match the panel shows the lobby's modifiers, locked (like the netplay profile rows).
- **When it applies:** read at fight start; changing the panel mid-fight takes effect at the next fight.

## Section 2: effects and determinism

Decided with Lane: Super Jump hook, turbo meaning. Decided by Bot while Lane was away (marked **[Bot]**, review these).

### Settings plumbing

- New key `Modifiers` in `wotm::settings` (`native/wotm_settings.hpp`), env `WOTM_MODIFIERS`, default 0. The value is a
  bitmask (`kModJump = 1`, `kModFast = 2`, `kModTurbo = 4`); `parse` reads the name list, the file writer writes the names
  back in canonical order. Load order stays default, file, environment, so the lobby's `WOTM_MODIFIERS` wins over the file.
  An environment variable that is set but empty means "none" (it must override a file that has modifiers on).
- **[Bot]** Network lock: `kNetKeys` holds fixed values today. `Modifiers` gets its forced value from the lobby instead: at
  `load`, with `WOTM_NET` set, the value from `WOTM_MODIFIERS` becomes the forced one and `forced(Modifiers)` is true. The
  panel shows the row locked, the file keeps the player's own choice.
- **Fight-start latch:** `wotm_sync::levelLoad()` (entry of `Shell::LoadLevelFiles`, already the fight-start anchor for the
  frame index) copies `settings::get(Modifiers)` into one runtime variable, `g_fightMods`. Every effect reads `g_fightMods`,
  never the live setting, so a panel change mid-fight cannot change the running fight.

### Super Jump (Lane: option A)

- Scale the global `TheGame::m_gravity` (`include/game/game.h`, offset 0x1203C4) by 0.45, once per fight, after the game has
  set it. One write; the game's own jump-height predictions (`StateJump`/`StateFly::getJumpHeightGain`,
  `src/game/MovementStates.cpp`), grapple-throw aim (`src/game/GrappleThrow.cpp`) and throw-back arcs all read the same value,
  so they stay consistent with the physics. Helicopters and homing bugs float too (fits "moon"). Weapons with their own
  gravity and particles are unchanged.
- **Open fact (first implementation task, measured, not guessed):** where the game writes `m_gravity` during a fight's setup.
  `TheGame::SetGravity` has no direct callers in the translated code, so the value is written elsewhere (or loaded). Find it
  with a write watch on that address during a level load, then apply the x0.45 right after that write. If the game writes it
  more than once per fight, the hook goes on that writer instead of a one-time write.
- Not hooked: `MonsterDynamics::m_gravityScale` (the game rewrites it itself, and the predictions ignore it).

### Fast Monsters ([Bot], option A of the last question)

The planned hook was wrong: `MonsterDynamics::getSpeedModifier` is only called by `StateDash::update` and
`StateRamAttack::update`. Walk and run go through `MonsterDynamics::updateFootMotion` (`src/game/MonsterDynamics.cpp`),
which applies rage and boost inline. So two hooks, both x1.5:

- Entry of `updateFootMotion`: multiply the target `speed` argument (`$f12`). Rage and boost stack on top; the minimum-speed
  clamp uses `runSpeed` from the config, which is unchanged, so slow walking still works.
- `getSpeedModifier`: multiply its return value, so dash and ram attack keep up with the faster run.
- Air motion, turning, animations and attacks: unchanged.

### Turbo (Lane: option A)

Turbo always means "2 fields per frame, each frame paced at 1 vsync", whatever the player count.

- **Fixed step (online, and offline determinism runs):** the server sends `step=2` when turbo is on (players 1-2: 1 -> 2;
  3+: already 2). The runtime paces each frame at `turbo ? 1 : step` vsyncs: the wait added in 6881c2e
  (`waitMinRateByVblanks`, `native/runtime/wotm_overrides.cpp`) uses that count instead of `g_fixedStep`. The game only sees
  2-field frames, its native 30 Hz mode, already proven identical Mac x Windows.
- **[Bot] Offline without fixed step:** the game's clock runs at 2x from the fight start: `hostTics()` as seen by the game
  becomes `base + 2 * (host - hostAtFightStart)` (continuous at the switch), and the vblank correction in
  `waitMinRateByVblanks` counts `2 * vblanks`. Everything that reads the clock (field counter, `timerGetCurTics`) speeds up
  together. Off again at the next fight start (same continuity rule).
- **[Bot] Sound stays at normal speed.** In fixed step, `wotmSoundFrame` advances by the frame's real vsyncs (1 in turbo), not
  by the step: mixing 2 fields of audio per 1 vsync of real time would overfill the output. It is still a function of frames
  only, so it stays deterministic. Offline, sound already runs on real time. Effect: music and effects play at normal pitch
  and length while the fight runs at 2x.
- Cost: 3+ player turbo draws 60 frames per second instead of 30; lockstep paces to the slowest PC.

### Determinism summary

Every effect reads `g_fightMods`, latched at the same frame index on every PC, and every modifier is in the config hash. The
gravity write and the speed multiplies are single float multiplies by constants in strict-FP code (`wotm_overrides.cpp` is
already built with strict FP), so x86 and ARM compute the same bits.

## Section 3: testing ([Bot])

Test-first for every piece. No mocks of the game: the effects are measured in the real game.

1. **Unit, native (`native/tests/wotm_native_test.cpp`, `tools/test_native.sh`):** name list parse (canonical order,
   duplicates, unknown name rejected, empty = 0); `settings.ini` round trip; empty env overrides the file; network lock (forced
   value = the lobby's, file value kept); config hash differs per modifier set and is the same for `turbo,jump` and
   `jump,turbo`.
2. **Unit, server (`native/net/wotm_net_test.cpp`, `tools/test_net.sh`):** `SET mods=` owner only, unknown name gives
   `ERROR`, `SETTINGS` and `MATCH` carry `mods=`, `step=2` for 2 players with turbo, `step=2` for 4 players with or without.
3. **Unit, lobby tool (`tools/test_net_lobby.py`):** `match_env` sets `WOTM_MODIFIERS` (empty when none); `form_group` can
   pass modifiers.
4. **Effects, measured (new coord script `scripts/modcheck.sh`, Windows twin `modcheck.ps1`):** replay the same recorded input
   (`WOTM_INPUTPLAY`, fixed step, seed 1234) with each modifier off and on. A debug log line (`WOTM_MODSTATS=1`, prints only
   numbers: player 1 peak jump height above the takeoff point and peak ground speed) gives the ratios. Pass: jump apex
   1.8x-2.6x, run speed 1.4x-1.6x. Needs a recorded input with jumps and a straight run (recorded once on the Mac, pushed to
   `runs/`).
5. **Turbo pacing:** `scripts/pacecheck.sh` gains `--turbo`: 60 frames per second and 120 fields per second, for 2 and 4
   players. Offline turbo: the same check without fixed step (fields per second about 120).
6. **Cross-machine:** stage 2 replay with all three on, Mac x Windows identical (`statediff.py`); one online match Mac x
   Windows with all three on, no desync; a join with different modifiers is refused (server unit test plus one real refusal).
7. **Lane playtest:** feel of the three fixed strengths. If one feels off, only its constant changes.

## Who does what

- Mac: settings key, runtime hooks, server, `net_lobby.py`, tests 1-5, docs (`docs/passos/NATIVO.md` variable table,
  `docs/ONLINE.md`).
- Windows chat: launcher (lobby settings checkboxes sending `SET mods=`, offline checkboxes writing `settings.ini`,
  `WOTM_MODIFIERS` from `MATCH`), `modcheck.ps1`, its half of test 6.

## Out of scope

Strength sliders, per-player modifiers, changing modifiers mid-fight, more modifiers (each new one follows the same pattern:
a bit, a name, a hook reading `g_fightMods`).
