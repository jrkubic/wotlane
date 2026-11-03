# The Vegas army as free-roaming entities: what's on the disc and what a spawn mod would cost (2026-10-25)

## The owner's question
On stage 2 of adventure mode (Vegas), after Togera is defeated, a cutscene brings in the army (tanks, helicopters, "anti-aircraft")
together with Robo-47. It only shows up there. Question: do these vehicles belong to the stage? And what would a mod cost that releases
them on any stage on a timer or by a chaos factor (buildings, cars and pedestrians destroyed)? The owner's decision: write it down, don't do it now.

## What's on the disc
A sweep of the node tree of the 19 `.NGP` files in `disc/LVL/` (same descent as `dbsTraverse`/`dbProcInteractive`, stopping at the
pickup node), looking for the ids that `LevelPickups::createPickup` (`src/game/Pickup.cpp`) turns into a military vehicle:

| Vehicle | Node id (type) | Stages | In Vegas |
|---|---|---|---|
| Tank (`TankPickup`) | 0x1392 (10) | VEGAS only | 8 |
| Missile truck (`MissileTruckPickup`), the "anti-aircraft" | 0x1391 (9) | VEGAS only | 4 |
| Military helicopter (`HeliPickup`, military) | 0x1394 (12) | VEGAS, METRO (12), TOKYO (6) | 5 |
| Turret (`TurretPickup`) | 0x13A0 | CANYON2 (2), UFO (6) | 0 |
| Flying saucer (`UfoPickup`) | 0x13A1 | ISLAND (7), UFO (5) | 0 |

- Each vehicle has its own mesh inside the stage's `.NGP`: tank 20 nodes and 3 objects (~20 KB of VIF packet), truck 18 nodes and
  3 objects (~7 KB), helicopter 40 nodes and 11 objects (~19 KB). Textures from `VEGAS.TEX`: 79 and 87 (tank and truck), 77, 78 and 88
  (helicopter). All the tanks in Vegas are the same model, and the same goes for the trucks.
- The military helicopter is byte-for-byte identical (md5 of the packets) in VEGAS, METRO and TOKYO; each stage carries its own copy. Four of
  the meshes are the same as those of CENTRAL's civilian helicopter (0x1393). The owner saw them in a cutscene in Metro; they don't remember
  Tokyo.
- `createPickup` only builds the tank, truck, military helicopter, turret and flying saucer when `TheGame::m_gameMode == 1`
  (adventure). Outside it the nodes stay in the file and the game creates nothing (which is why they don't appear in free-for-all or through
  `WOTM_MATCH`, mode 2).

## How Vegas uses them
- When adventure mode loads they're created and kept hidden in the list of active pickups (`LevelPickups::activeList`).
- `vegasUpdate` (the stage script) calls `LevelPickups::initMilitaryForVegas`: two `BoxFormation(2, 83, 150)` with 4 tanks each
  (leader + members), 2 trucks in each, helicopters in state 0, `drawMe` 1. It also calls `turnOffMilitary`.
- `turnOffMilitary` / `turnOnMilitary`: types 5 and 10 (tank), 9 (truck), 11, 12 and 23 (helicopters) go to the idle state
  (7; 5 for the helicopter) or to state 0, with `drawMe` 0 or 1. It's a ready-made pool: hide and wake.
- `_12LevelPickups$s_militaryStillAlive` is set by `HeliPickup::update` while any military unit is alive (the Vegas script reads it).

## What each vehicle depends on
- **Navigation:** `TankPickup` and `MissileTruckPickup` set up the `VehicleNavigator` on `PathNet::fodder` (the network for cars and
  pedestrians; `lui %hi(_7PathNet$fodder)` in the constructor). So does the helicopter: `HeliPickup` passes `&PathNet::fodder` to
  `HeliVehicleNav(HeliVehicle&, PathNet*)`, which has `followPath`, `getRandPath`, `getSeekPath` and `getFleePath` over it (it isn't free
  flight). The cars (`CarPickup`) and the flying saucer (`UfoPickup`) use the same network.
- **Where the network comes from:** `TheGame::InitBeforeDbLoad` starts `PathTool::instance` (name `fop`, folder `pts`), which reads
  `disc/PTS/<STAGE>.FOP`, and `exportPoints` copies it into `PathNet::fodder` (up to 256 nodes). 13 stages have a `.FOP`: 3-MILE, AIRPORT,
  CANYON2, CAPITOL, CENTRAL, ISLAND, METRO, MINISF, SANFRAN, TOKYO, TOKYODM, UFO and VEGAS. No `.FOP`: BIGSHOT, CANYON1, CRUSH,
  DBALL, TRAINING and VOLCANO. With zero points `exportPoints` builds a fallback network of 2 nodes, (-100, 0, 250) and (100, 0, 250),
  linked to each other (not checked whether `PointToolKit::loadPoints` leaves zero points when the file is missing). On those 6 stages a
  released vehicle would only drive along that segment; the mod would need to bring its own `.FOP` (the format is the one from `PathTool::exportPoints`).
- **Shooting:** `TankVehicle::updateControls` fires with `Weapons::CreateStaticWeapon` (type in the vehicle's +0x60 field). No weapon
  (ids 0xFA0..0x11F7) is exclusive to Vegas: the projectile models are in almost every stage.
- **Sound:** `TankSound::updateTankSound` plays sound 0xAA from the bank with index 5 (`gSoundBanks`). Not checked whether 5 is the stage's bank
  (`VEGAS_S`) or a shared one; if it's the stage's, outside Vegas it comes out silent or wrong. A precedent for solving it: the pause panel
  loads a copy of `SHELL_S.BNK` (`wotm_snd_hooks::panelShellBank`).
- **Memory and slots:** the constructors allocate from `MemoryStack::global` and `initAfterDbLoad` takes a slot in `Interactives`
  (`addInteractive`, the index goes to the node above). That's why the pool has to be created at load time, not in the middle of the fight, and each
  instance needs its own copy of the node above (the meshes can be shared).

## Proposed plan (in stages)
1. **Vegas and the Metro/Tokyo helicopters outside adventure mode (cheap, ~1 day).** A hook in `createPickup` that fakes mode 1 only
   for the military ids; after the load, everything as `turnOffMilitary` leaves it. A mod timer wakes N of them (`setTrans` on a node
   of `PathNet::fodder` + `enterState(0)` + `drawMe` 1, like `turnOnMilitary`). No file changes; it serves to prove that the
   AI, shooting and sound work outside adventure mode before the rest.
2. **Chaos factor (cheap).** Hooks on a destroyed building (`Destructibles`), destroyed car (`CarPickup::kill`) and killed
   pedestrian add up points; each threshold releases a wave.
3. **Tank and truck on any stage (medium to hard, several days).** An offline tool (`tools/ngp.py`, `tools/tex.py`) extracts
   the nodes of the three vehicles and their textures from VEGAS into a mod file; at load time, a hook copies them into high RAM (the one from
   `WOTM_HIGHRAM`), rebases the pointers (the stage image is prelinked at 0xA00000), uploads the textures and creates the pool. The
   risky part is the texture: free space in VRAM and the `TBP0` of each object's `TEX0`. It's related to the "own files" of the
   custom monsters (`TODO.md`, item 8), which also need to load an extra model.

## How it was measured
Scratch scripts (outside the repo) on top of `tools/ngp.py`: a generic descent through the children of each node type (GROUP, LOD, SWITCH,
TRANSLATE, ROTATE, SCALE, CONTROL, ACTION_DATA, CHAR_INSTANCE, ANIM_XFORM, ANIM_SCALE, INTERACTIVE_STATE, DESTRUCTIBLE), a count
of ids 0x12C0..0x13EB and 0xFA0..0x11F7 per stage, md5 of each object's VIF packet (`polyPkt`, `QWC + 1` quadwords) to compare
models across stages. Calls and targets read from the asm of `TankPickup`, `TankVehicle`, `MissileTruckPickup`, `vegas` and `Sound`.
