# Gameplay Modifiers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Super Jump, Fast Monsters and Turbo as on/off fight modifiers, chosen in the lobby or offline, identical on every PC.

**Architecture:** One pure header (`native/net/wotm_modifiers.hpp`: names, bits, list parse/format, constants, clock and pacing
math) shared by the runtime, the settings and the lobby server. The runtime latches the active set at fight start and hooks
three game functions; turbo changes fields and vsyncs per frame (fixed step) or the clock rate (offline).

**Tech Stack:** C++17 (runtime hooks, native tests, `wotm_server`), Python 3 (`tools/net_lobby.py`), C# launcher (Windows chat).

**Spec:** `docs/superpowers/specs/2026-11-02-gameplay-modifiers-design.md`

## Global Constraints

- Names and canonical order: `jump,fast,turbo`; empty list = none; an unknown name is an error everywhere.
- Strengths: gravity x0.45, foot speed/accel and dash modifier x1.5, turbo = 2 fields per frame at 1 vsync per frame.
- Every effect reads the modifiers latched at fight start (`Shell::LoadLevelFiles` entry) and cleared at `rtReturnToShell` entry.
- English everywhere; addresses by name (`wotm::fn::`/`wotm::sym::`); new env variables go in the NATIVE.md table.
- Config hash includes the canonical list from `WOTM_MODIFIERS`.

**Change vs spec (simpler, same behavior):** the server does not change `step`. During a turbo fight the runtime uses 2 fields
and 1 vsync per frame whatever `WOTM_FIXEDSTEP` is; between fights the normal step comes back, so menus never run at 2x.
Settings keep three on/off keys (`WOTM_SUPERJUMP`, `WOTM_FASTMONSTERS`, `WOTM_TURBO`: one panel row each, like every other
option); `WOTM_MODIFIERS=<list>` (lobby, launcher) sets all three and, in a network match, locks them.

## Review Focus

1. `WOTM_MODIFIERS` unset in a network match must mean "none", not the player's file values (else configs differ -> refused).
2. Turbo must end at the fight's end: post-fight menus at normal speed (latched off at `rtReturnToShell`).
3. Super Jump applied twice (level reload / round reset) would compound: never scale a value we already scaled.
4. Offline turbo clock switch must be continuous (no jump of the field counter at fight start or end).
5. A lobby `SET mods=bogus` must leave the lobby's modifiers unchanged and answer `ERROR`.

---

### Task 1: shared modifier definitions

**Files:** Create `native/net/wotm_modifiers.hpp`; Test `native/tests/wotm_native_test.cpp`.

**Produces (namespace `wotm::modifiers`):** `enum Bit { Jump = 1, Fast = 2, Turbo = 4 }`; `kNames[3] = {"jump","fast","turbo"}`;
`bool parse(const std::string &list, int &bits)` (comma list, any order, duplicates ok, spaces trimmed, empty -> 0, unknown -> false);
`std::string format(int bits)` (canonical order); `kJumpGravity = 0.45f`, `kFastSpeed = 1.5f`;
`int fieldsPerFrame(int step, int bits)` (turbo -> 2, else step); `int vsyncsPerFrame(int step, int bits)` (turbo -> 1, else step);
`uint32_t scaledClock(uint32_t host, uint32_t host0, uint32_t game0, uint32_t scale)` = `game0 + (host - host0) * scale` (wraps).

- [ ] Test `modifier_list_parse_and_format`: `parse("turbo,jump")` -> 5 and `format(5) == "jump,turbo"`; `parse("")` -> 0;
  `parse("fast, fast")` -> 2; `parse("moon")` false; `fieldsPerFrame(1,Turbo)==2`, `fieldsPerFrame(2,0)==2`,
  `vsyncsPerFrame(2,Turbo)==1`, `vsyncsPerFrame(2,0)==2`; `scaledClock(110,100,500,2)==520`; wrap: `scaledClock(5,0xFFFFFFF0,0,2)==42`.
- [ ] Run `sh tools/test_native.sh`: fails (header missing). Implement. Run: passes. Commit.

### Task 2: settings keys, network lock, config hash

**Files:** Modify `native/wotm_settings.hpp`, `native/runtime/wotm_sync.inc` (`netConfigHash`); Test `native/tests/wotm_native_test.cpp`.

**Produces:** keys `SuperJump`, `FastMonsters`, `Turbo` (env/file `WOTM_SUPERJUMP`, `WOTM_FASTMONSTERS`, `WOTM_TURBO`, default 0);
`inline std::atomic<int> g_netMods{0}`; `netValue(k)` returns the bit of `g_netMods` for those keys (so `forced()` locks them in a
network match); `inline int modifiers()` = bits from `get()` of the three keys; `applyModifierList(const char *list)` (sets the
three `g_val`); in `load`: `WOTM_MODIFIERS` applied after the other env; with `WOTM_NET`, `g_netMods` = parse of
`WOTM_MODIFIERS` (unset -> 0) and the file values stay in `g_val`. `netConfigHash` adds `modifiers=<format(parse(env))>;`.

- [ ] Test `modifier_settings_and_net_lock`: with `g_val` jump=1, `g_netProfile=false` -> `modifiers()==Jump`; `g_netProfile=true`,
  `g_netMods=Turbo` -> `modifiers()==Turbo`, `forced(SuperJump)`, `g_val[SuperJump]` still 1; `parse(Turbo,"1")` -> 1.
- [ ] Fail, implement, pass, commit.

### Task 3: runtime effects

**Files:** Create `native/runtime/wotm_modifiers.inc` (included by `wotm_overrides.cpp` after `wotm_sync.inc`); modify
`wotm_overrides.cpp` (clock, pacing, install), `wotm_sync.inc` (`frame` uses `fieldsPerFrame`, sound uses `vsyncsPerFrame`).

**Produces:** `wotm_modifiers::g_fight` (bits, EE thread); `fightStart()` (from `wotm_sync::levelLoad`), `fightEnd()` (wrapper on
`rtReturnToShell`, chained over whatever is registered), log lines `[modifiers] fight: <list>` / `[modifiers] off`;
hooks: `InitAfterDbLoad__7TheGame` entry scales `game->m_gravity` (`*(game)+0x1203C4`) unless it equals the last value written;
`updateFootMotion__15MonsterDynamicsfff` entry scales `$f12` and `$f13`; `getSpeedModifier__15MonsterDynamics` scales `$f0` after
the call. Offline turbo: `hostTics()` seen by the game goes through `scaledClock` with scale 2 between `fightStart` and
`fightEnd`; the vblank correction multiplies by the same scale. `WOTM_MODSTATS=1`: every 300 frames,
`[modstats] frame N peak_height H peak_speed S` for `m_monsters[0]` (height = max z - min z of the cs trans, speed = max |vel.xy|
at Monster+0x1E0).

- [ ] Build (`sh tools/build_runtime.sh`), then `scripts/modcheck.sh` (Task 5) is this task's test. Commit.

### Task 4: lobby server and net_lobby.py

**Files:** `native/net/wotm_net_server.{hpp,cpp}`, `native/net/wotm_net_test.cpp`, `tools/net_lobby.py`, `tools/test_net_lobby.py`.

**Produces:** `Lobby::mods` (canonical string); `SET mods=<list>` (owner only; invalid -> `ERROR unknown modifier in "<list>"`,
nothing changes); `SETTINGS ... mods=<list>` and `MATCH ... mods=<list>`; `Lobby.set(..., mods=None)`, `match_env` ->
`WOTM_MODIFIERS` (always present), `form_group(..., mods='')`, CLI `--mods`.

- [ ] Tests: server `lobby_modifiers`: SET by owner -> SETTINGS has `mods=jump,turbo` for input `turbo,jump`; non-owner ERROR;
  `mods=moon` ERROR and unchanged; MATCH has mods. Python: `match_env` sets `WOTM_MODIFIERS` ('' when the MATCH has none).
- [ ] Fail, implement, pass (`sh tools/test_net.sh`, `python3 tools/test_net_lobby.py`), commit.

### Task 5: panel tab and docs

**Files:** `native/runtime/wotm_mainmenu.inc` (tab `MODIFIERS` between CAMERA and CONTROLS, three rows, hint "TAKES EFFECT AT
THE NEXT FIGHT" in each), `docs/en/steps/NATIVE.md` (variables), `docs/en/ONLINE.md` (SET mods, MATCH mods).

- [ ] Build, open the panel with `WOTM_PADSEL_SCRIPT` to the new tab and toggle a row; `settings.ini` gets `WOTM_TURBO=1`. Commit.

### Task 6: measured effects and cross-machine (coord)

**Files:** coord `scripts/modcheck.sh` (Windows twin `modcheck.ps1` by the Windows chat), `scripts/pacecheck.sh --turbo`.

- [ ] `modcheck.sh`: replays `runs/2026-10-30-framehash/windows/stage2/inputs.bin` with `WOTM_MODSTATS=1` for off/jump/fast;
  pass: height ratio 1.8-2.6, speed ratio 1.4-1.6. `pacecheck.sh --turbo`: 60 frames/s and 120 fields/s at 2 and 4 players.
- [ ] Mac x Windows: stage 2 replay with `WOTM_MODIFIERS=jump,fast,turbo` identical; online match with all three, no desync;
  join with different modifiers refused. Launcher half (Windows chat): lobby checkboxes, offline checkboxes, `WOTM_MODIFIERS`.
