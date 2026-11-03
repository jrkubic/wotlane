# Online camera (mouse and right stick): design

Date: 2026-11-03. Branch: wotlane/mac (work on wip/mouse-online). Status: written for Lane's review (not approved yet).

## Goal

In an online match each player steers their own modern (orbit) camera with the mouse or a gamepad's right stick, using their own
camera settings, and every PC still computes the same game state every frame.

Lane's decisions:
- **Design A:** each player's own settings travel with that player's input. Every PC applies player N's camera input with player
  N's settings, never its own.
- **Old input recordings keep loading** (backward compatibility approved by Lane): recordings made before this change replay with
  the same result as today.

## Why the camera has to be synced

The orbit camera (`native/runtime/wotm_orbit.inc`) is part of the simulation: the monster walks relative to the camera, and the
camera writes the game's camera state. Every PC runs the orbit for every player slot 0..3 from that slot's synced input. Today:
- the right stick travels in `PadState` (raw `rx`/`ry`), but the orbit scales it with **this PC's** `CamSens`/`CamInvertY`;
- the mouse never travels: only the local keyboard player's orbit takes the live motion (`wotm::mouse::take`);
- so online play forces the classic camera (`kNetKeys`/`kNetValues` in `native/wotm_settings.hpp`).

The draft on wip/mouse-online (3eca24a) sends the mouse but still scales it with this PC's settings: two players with different
sensitivities would desync. This design fixes that by sending the settings too.

## Section 1: what travels per frame

`wotm_net::PadState` (`native/net/wotm_lockstep.hpp`) grows from 6 to 12 bytes:

| Field | Type | Meaning |
|---|---|---|
| `buttons`, `rx`, `ry`, `lx`, `ly` | as today | pad state |
| `mouseX`, `mouseY` | int16 | mouse motion of this controller's player on this frame, whole pixels (fraction carried to the next frame, as in 3eca24a); 0 for controllers no mouse feeds |
| `cam` | uint16 | that player's camera settings, packed (below) |

`cam` bits (`wotm::camsettings`, a new header `native/net/wotm_camsettings.hpp` with `pack`/`unpack`, no game dependency):

| Bits | Setting |
|---|---|
| 0 | present (1 = the fields below are valid) |
| 1 | `Camera` (1 = modern orbit camera) |
| 2 | `CamRecenter` |
| 3 | `CamInvertY` |
| 4..7 | `CamSens` (1..10) |
| 8..15 | 0 (reserved; must be 0) |

- **Sampling** (`wotm_sync.inc`, `sampleInputs`, fixed step only): every sampled controller gets `cam` = this PC's current settings
  with `present` = 1. The keyboard player's controller also gets the mouse motion. Online that is game pad 0 (this PC's player);
  offline the keyboard player (`g_kbPlayer`).
- **Applying:** the orbit reads slot N's settings with a helper `camSettingsFor(slot)`: from `inputFor(slot, g_index)` when
  `present` = 1, else this PC's settings (old recordings, and the non-fixed-step offline path that has no timeline). It replaces
  every `wotm::settings::get/on` of those four keys in the orbit: `enabled()`, `eligible()`, the stick and mouse scaling, recenter.
- **Mouse in the orbit:** with the fixed step, slot N's mouse comes from its input (3eca24a's change); without it, the live
  keyboard player's motion as today. The cursor capture (`wotm::mouse::want`) stays local: only this PC's player.
- **Changing settings mid-fight** (pause panel) takes effect at the next sampled frame on every PC alike, since it travels.
- **Gamepad:** the right stick already travels; with `cam` it is scaled by its owner's settings, so a controller works online with
  no extra change.

## Section 2: what changes elsewhere

- **Protocol 3** (`wotm_net_proto.hpp`): `pad()` writes and reads the 12 bytes; an unchanged state still costs 1 byte. Worst case
  (mouse moving every frame): 13 bytes per player per frame, about 0.8 KB/s at 60 frames/s. The server refuses protocol 2 clients
  with its existing protocol error. `LobbyClient.Protocol` (launcher) and `tools/net_lobby.py` move to 3 (df84a9f already makes
  the launcher's own row use the constant).
- **Network profile** (`wotm_settings.hpp`): `Camera`, `CamRecenter`, `CamSens`, `CamInvertY` leave `kNetKeys` (each player picks
  them). `LockToggle` (0) and `CloseCam` (1) stay forced: they change how the game reads buttons and the shared unified view, which
  is outside this change. The config hash doesn't include camera settings today (only env vars and modifiers), so it doesn't change.
- **Players 5..8** (network players in AI slots): the orbit only runs for slots 0..3, so they keep the classic camera. Unchanged.
- **Input recordings** (`WOTM_INPUTREC`/`WOTM_INPUTPLAY`, `wotm_sync.inc`): today a file is raw 16-byte records
  `{u32 index, u32 controller, PadState(6) + 2 padding}`. New files start with an 8-byte header `WOTMINP` + version byte `2`,
  followed by 20-byte records `{u32 index, u32 controller, PadState(12)}`, written field by field (no struct padding). A file
  without the header is read as the 16-byte format with `mouseX/Y` = 0 and `cam` = 0 (not present, so the replaying PC's settings
  apply, exactly as today). The reader is a pure function (`wotm_net::readInputRecords(bytes) -> records`, in `wotm_lockstep.hpp`
  or a small header next to it) so the native tests cover both formats. An unknown version prints an error and loads nothing.
- **Docs:** ONLINE.md (mouse and right-stick camera work online, each player's own settings), NATIVE.md/NATIVO.md (recording
  format), RUNTIME.md if it lists the network profile.

## Section 3: testing

Unit tests (test-first):
- net tests: PadState with mouse and `cam` round-trips through encode/decode, including extremes (-32768, 32767, every `cam` bit);
  a changed `cam` alone makes a new state; a protocol 2 HELLO is refused.
- native tests: `camsettings::pack`/`unpack` round-trip for every sens 1..10 and flag; `unpack(0)` = not present;
  `readInputRecords` reads an old-format buffer (mouse 0, cam 0) and a new-format buffer, and rejects an unknown version.

Runtime checks (real game, no mocks):
1. **Old recordings unchanged:** the stage-2 replay (runs/2026-10-30-framehash, old format) gives the same hash as before on the
   Mac, and the modifiers replay (runs/2026-11-02-modifiers) too.
2. **Mouse replay, Mac x Windows:** Windows records a fight with real mouse motion and non-default settings (sens 8, inverted) using
   `tools/mouse_check.ps1`; the Mac replays the file and `statediff.py` finds them identical. The Mac also replays it with its own
   settings set differently (sens 2, not inverted): still identical, which proves the recorded settings win.
3. **Local online match** (2 clients on the Mac, different sens/invert, mouse on one): no desync.
4. **Mac x Windows online match** with the mouse on Windows and the stick on the Mac: no desync.
5. **Lane and Jake** play over the internet with the mouse camera.

## Split of the work

- **Mac:** this spec, `wotm_camsettings.hpp`, PadState/protocol/lockstep + net tests, the recording format + native tests,
  net_lobby.py/launcher protocol constant, coord scripts, checks 1 and 3, the Mac side of 2 and 4.
- **Windows:** the runtime side on wip/mouse-online: sampling (`cam` + mouse), `camSettingsFor` in the orbit, the network profile,
  `keyboardPlayer()` online (3eca24a), `mouse_check.ps1`, the Windows side of 2 and 4, then rebuilding for check 5.

## Out of scope (YAGNI)

- Per-player `LockToggle` and `CloseCam` online.
- The orbit camera for players 5..8.
- A pause that doesn't stop the match for everyone (separate design item from the first internet match).
