# 3D monsters straight from the disc (pose, skeleton, textures)

(Index of the docs in `CLAUDE.md`.) How to assemble an animated monster from the `MON/` files alone, without the game or a RAM dump. It serves the save list
(monster idling over the stage photo, `NATIVO.md`, "Save in JSON"), the 3D preview on the select screen (item 12) and the web viewer (`VIEWER_WEB.md`). Base formats (mesh, curves) in `docs/FORMATOS.md`.

## Code
| Where | What |
|---|---|
| `tools/animcurve.py` | curve evaluator (port of `src/common/AnimCurve.cpp`) |
| `tools/pose.py` | a monster's pose (`Model(ngp).pose(anim, frame)`), `--list` shows the animations; writes a posed `.glb` |
| `tools/gltf.py` | exports `.glb` (textures from `tex.py`); with `model=` the skinned meshes come out in the pose |
| `tools/monglb.py` | animated `.glb` for the web viewer: joints = bones with an output (parent = whoever wrote the starting matrix on the stack), inverse bind = `restPoseInv`, rigid parts as nodes; curves sampled at 60/s; GS blends, scrolling texture, fire and lens flare in the extras. `--boss plant|root|mecha` assembles the bosses from the stage pieces (3-MILE, CAPITOL). Matches `pose.py` vertex by vertex |
| `tools/shellpics.py` | wide stage photos and monster logos from `SHELL.TEX/.RTX` as PNG (the same ones as in `wotm_savepics.hpp`) |
| `native/wotm_model.hpp` | the same in C++: `wotm::model::Model` (load, pose, posed triangles, textures) and `Renderer` (raylib, RenderTexture) |
| `native/monview.cpp` | `build\monview.exe disc MONKEY0 [--anim 90] [--yaw 35] [--shot x.png]`: animated window (arrows rotate, PgUp/PgDn switch the animation) or PNG |

In the save list: `wotm_save.inc` posts a `savepics::kMonster` (monster | costume << 8); the `Drawer::prepare` of `wotm_savepics.hpp`
draws the monster into the RenderTexture before the frame (`wotm_live.inc` calls it), and `draw` pastes the texture between the frame border and the text.

## Files
- `MON/<PREFIX><costume>.NGP` (raw, not zipped), `.TEX`, `.RTX` (raw for the monsters) and `.PTR`. Prefixes: MONKEY Congar, ROBOT Robo-47,
  LIZARD Togera, SHOGUN Ultra-V, MANTIS Preytor, ENERGY Kineticlops, LAVA Magmo, DRAGON Raptros, ROCK Agamo, ALIEN Zorgulon, ASSBOSS
  Goliath Prime. Costume 0 to 3 = `Shell::m_costume` (`Shell::formatFilename1`).
- **The `.NGP` comes prelinked for 0xA00000.** Read at that base, the pointers are valid without the `.PTR` (the `.PTR` only shifts them to where the game
  loaded it). The native layer puts the file in a buffer at 0xA00000 and uses the usual `Ram` (`decodeObject` works unchanged).
- Root 0 = the monster (GROUP with the monster's id); root 1 = GROUP id 0 (HUD portraits and extras); then COLL_GRID and ANIM_MGR.

## Animation
- Main instance: the `CHAR_INSTANCE` (op 25) reachable from root 0 with the most animations. `character` at +0x20 (`numAnims` u16 @0,
  `numChannels` u16 @2, `animations[]` @0xC); initial channel values at `ci+4`.
- Animation `id` = the one in `animations[k]` with `head >> 18 == id` (the Monster stores the k-th as `m_anims[id]`, ids 0..299,
  `Monster::initAfterDbLoad`). `startTime` @4, `endTime` @8, `numChannels` u16 @0xC, curves @0x18. **Idle = 90 (0x5A)**
  (`StateIdle::chooseBaseIdle`; 0x5F tired, 0x60 with special). Time in frames at 60/s; Congar's idle is 160 long.
- Each curve writes `val[dataIdx]` (FORMATOS.md, "Animation"). Matches `animOutput.val` from a fight in 128 of 130 channels (the other 2, 0.007, come from a blend layer).

## Traversal (part matrices)
Row vectors (`p' = p·M`), child = `local · parent`. Per node:
- **ANIM_XFORM (17):** visible = `+0xC`, or `val[visidx]` (`+0x10`) if the channel was written; 0 = cull. Rotation = `-val` of the
  rX/rY/rZ channels (`+0x12..0x16`; with no channel, the vector at `+0x20`), matrix `mathfRotMatrixPRH` (= VU0 microprogram 0x4D0). Bits in
  `+0xC2`: bit 0 `L = pre·L` (`+0x40`), bit 1 `L = L·post` (`+0x80`). Then `m[3] += translation` (channels `+0x18..0x1C` or `+0x30`).
  Children at `+0xCC`, count at `+4`.
  **It only rebuilds if some channel is dirty:** `firstIdx` (s16 `+0x1E`) and the `animating` mask (u8 `+0xC1`); it rebuilds if some bit
  `b` of the mask is set and channel `firstIdx+b` was written. Otherwise the matrix is the file's, `atMat[matrixIdx]` (`ci+0xC`,
  index at `+0xC4`). On Robo-47 (`ram_1100`) 35 are rebuilt and 5 come from the file; without the rule the claws
  (indices 27 and 39) come out turned around.
- **ANIM_SCALE (38):** reads `val` directly (without checking whether it was written); `pre(+0x40) · scale · post(t + 0x50)`; zero scale culls.
- GROUP (bit 0 of `+0xA`), LOD (the most detailed one `hierLod` would draw up close: the highest index with `switchOutDis` (`+0x24 + 16i`) > 0; Togera's costume 3 has a last level with 0, never drawn, and taking the last one gave the coarse mesh in A-pose), TRANSLATE, ROTATE, SCALE, SWITCH, CONTROL/ACTION_DATA,
  INTERACTIVE/DESTRUCTIBLE as in the native `Scene::walk`.

## Skeleton and palette
- **SKEL_BONE (35):** `numKids` +4, `skelOutputMatIdx` s16 +8, `isRootNode` +0xA, children +0xC, `restPoseInv` +0x10. Pushes the bone
  (`si+1`); if it is a root, also pushes an identity (`mi+1`).
- The **next transform node** (ANIM_XFORM, TRANSLATE, ROTATE) with `si >= 0` pops: `smat[mi+1] = local · smat[mi]`,
  `palette[bone.out] = restPoseInv · smat[mi+1]`, `si-1`, `mi+1`. The state is passed by value to each subtree.
- Address of each output: `animMatrixPtr[out]` (`ci+0x14`). The skinned OBJECT finds its block through `animPktPtr` (`ci+0x18`)
  `[OBJECT+0x68]` (DMA ref tag, +4 = block), matrix of slot `s` at `block + 0x10 + 0x40·s`.
- **Skinning:** 3 bones per vertex, `w1 = w`, `w2 = frac(w·2048)`, `w3 = 1 - w1 - w2`, slot = `(code & 0x7F) / 4`; then the object's
  matrix. The normal is rotated only by bone x.
- The assembled palette equals the one in a fight dump (`ram_1500`). On other frames the RAM palette is one frame behind
  `atMat` (not a model bug).

## Textures
- OBJECT slot `s`: texId at `+0x50 + 2s`, TEX0 at `+0x70 + 0x30s` (how many: `+0x5A`, up to 5); the section's slot comes from the GIF tag
  (`Vtx::slot`).
- **For the monsters the numbers in the `.NGP` are already the file's own:** texId = the one of the `.TEX` packet (or of the `.RTX`, e.g. texId 2 T8H
  for Congar), CBP = `tbp` of the `.RTX` CT32 palette. Zero offset in the 10 monsters and in the costumes (`tex.Pairing` finds 0/0).
- Linear pixels: `.TEX` at `+0x80`; `.RTX` at `+0x80` for the high formats (T8H/T4HL/T4HH) and `+0x10` for the rest. 256-entry palette with the
  CSM1 swap (bits 3 and 4) only on T8; T4 uses `csa·16 + k` into a 256-entry palette. GS alpha: `>= 0x80` = 255, otherwise `·2`; TCC 0 = opaque.

## GS state per node (blending, alpha test, Z)
Like the native `Scene::groupState`: the context is at `+4` (GROUP, TRANSLATE, ROTATE, SCALE) or `+8` (ANIM_XFORM, ANIM_SCALE); it applies
to the subtree. A+D packet with `+0x14 == 0x10000000`, `+0x18 == 0xE`, NLOOP at `+0x10`, pairs from `+0x20`: ALPHA_1 (0x42,
low byte), TEST_1 (0x47: ATE bit 0, ATST bits 1-3, AREF bits 4-11), ZBUF_1 (0x4E, ZMSK). Default: `0x44`, no test. Blending only
applies with the mesh's PRIM.ABE. Modes seen: `0x44` normal, `0x48` additive (Kineticlops), `0x42` `Cd - Cs·As`, `0x46` `Cd·(1-As)`,
`0x81` `(Cd-Cs)·As`. The `Renderer` reproduces them with premultiplied color (opaque ones first, then the ABE ones in model order) and
draws the background inside the RenderTexture for the modes that depend on the destination. UV offset: `+0x20/+0x24` of the node, or the
channels `+0x30/+0x32` with bit 0x800 in `+0xC`. On Kineticlops (costumes 0-2) those channels (142/143) come from animation 0x91, which
has only those two curves and loops on top of the main one (the idle writes 0 to them): U makes one full turn every 80 frames, V two.
In the fight dump `build/dumps_live/ram_energy_robot.bin`, U and V are 0.637 and 1.275 (ratio 1:2).

## Framing
`Model::computeFraming(id)` samples the whole animation and fixes the box; without it the camera follows the idle ("dancing" zoom).

## Fire and lens flare
The effect textures come from `LVL/CENTRAL.TEX/.RTX` (`FxTextures`): Fireball = texId 5 (palette tbp 13); flare = texIds 54/4/55/56
(palettes 203/9/207/211, texInfo 15/17/18/19). Reverse-engineering details in `75f5be1:docs/investigacoes/2026-10-21/fogo_agamo.md` and
`75f5be1:docs/investigacoes/2026-10-21/lens_flare_monstros.md`.
- **Fire (node op 22, HierParticleEmitter, preset +4 = 141 IdolHeadFlame).** Emitter E = position(+0x10)·m. Additive sprite particle,
  life 20 fields, size 7 + 8U, growth 1 − 1.5·min(age/10, 1), wind (0,0,3,2), drag 0.0390625, 2 per field, color from a
  16-entry table (age·3072 in 4.12), UV mirrored by 2 bits. A new system starts with 30 warm-up steps. Framing
  stretches the box 55 units above the emitter. Only 141 is simulated (AttachedFlames and the flying head are not).
- **Lens flare (node op 5, HierLight with isOn and hasLensFlare).** P = pos·m, D = beam·m; brightness I/W², size √I/W; directional:
  k = max(dot,0) + bw − 1, scale k/bw. 18 fixed sprites along C + t(S − C), additive, after Mode3D, no Z test
  (occlusion not reproduced: always visible). Monsters with a light: ROBOT0–2, MONKEY1–3, JELLY0, ASSBOSS1.

## Not reproduced
Fur and tail springs (`BoneSpring`: they stay in the animation's pose), procedural look/head, animation blend layers and the game's
lighting (the `Renderer` uses a fixed directional light + ambient over the vertex color).

## For the select-screen preview and the web viewer
- Select screen: the same `Model` + `Renderer` (one per player); changing costume = reloading the `.NGP` (~1 MB, plus a 10 MB buffer).
- Web: port `tools/pose.py` + `animcurve.py` (small) to JS; the textures and mesh follow `tools/gltf.py`.
