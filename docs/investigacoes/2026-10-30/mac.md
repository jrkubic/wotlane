# macOS port with Windows x Mac online play: what decides determinism

Lane's question: port the game to his Macs (MacBook Neo and MacBook Pro, all Apple Silicon) and play online Mac against Windows,
with either side hosting. Decisions: a native arm64 build with clang (SSE through the `sse2neon.h` that PS2Recomp already ships);
no Rosetta (Apple keeps it complete only through macOS 27) and no Wine. The Mac build itself lives on `wotlane/mac` (ported by the
Mac session); this report covers why two machines with different compilers and CPUs computed different game states, and what
fixed it. Live coordination between the Windows and Mac sessions: the private repo `sightama/wotlane-coord` (`STATE.md`, `log/`,
`runs/`).

## Deterministic by construction

- **Translated FPU (COP1) code:** `FPU_ADD_S/SUB_S/MUL_S` (`ps2_runtime_macros.h`) pass every operand and result through
  `ps2_fpu_fix`, which reads the float's bits, so no compiler can fuse or reorder across it. `sqrtf`, division and
  `roundf/floorf/ceilf/nearbyintf` are exact in IEEE. The R5900's `madd.s` becomes `FPU_ADD_S(acc, FPU_MUL_S(...))`: two roundings.
- **`rand`/`srand`:** the runtime's own newlib LCG (`LibC.cpp`), not the system's.
- **Network protocol:** `wotm_net_proto.hpp` writes byte by byte in little-endian; `native/net` builds warning-free with gcc and its
  254 tests pass, so a Linux/Mac `wotm_server` and the Windows one are interchangeable (proven: a Mac joined a Windows-hosted
  lobby over the LAN).

## What made Windows and Mac diverge, and the fixes

| Cause | Fix | Commit |
|---|---|---|
| The recompiler stubs every name in the runtime's `ps2_call_list.h` (config.toml can't override it). `__kernel_sinf`/`__kernel_cosf` went to the host `::sinf/::cosf` and `__ieee754_rem_pio2f` to a crude reduction compiled with `/fp:fast`. Microsoft's and Apple's libm differ in the last bit: on stubbed builds Windows matched retail on 9 of 85 values, the Mac on 7, and they disagreed with each other. | Remove the three names from the call list so the game's own newlib fdlibm is translated like the rest of the game. `WOTM_TRIGCHECK=1` checks `sinf`/`cosf` against 85 retail values (the retail code run in unicorn, `cvt.w.s` as `trunc.w.s` because the R5900 truncates): 85 of 85 on both machines. | c8ced22, b6de417 |
| MSVC `/fp:fast /arch:AVX2` fused 588 multiply-adds into FMAs in 192 translated game functions (particles, cameras, collision, `MonsterDynamics::updatePosition`...): the VU0 macro ops are separate `_mm_mul_ps` + `_mm_add_ps` with no bit barrier. An FMA rounds once where the PS2 and clang/arm64 round twice. | `EnableFastReleaseMode(ps2_game_code STRICT_FP)`: `/fp:precise` on MSVC, `-ffp-contract=off` on gcc/clang, for the translated game code only (the runtime and native layer keep `/fp:fast`). FMAs in translated code: 588 -> 0, same frame rate. | 1adff9c |
| The rumble code installed its pad hooks only on machines with a gamepad, so the game's pad buffers (0x77A0C0-0x77A280) differed from frame 0. | Install the pad hooks on every machine (Mac session). | 40bedba |
| VU0 macro ops (`PS2_VADD/VSUB/VMUL/VDIV/VMULQ`) produced NaN/inf on ARM where the PS2 gives +-Fmax (seen in `PedGroup::update`). | Apply the FPU's rule per lane (`ps2_vu_fix`, integer SIMD, bit-identical on x86 and ARM; x/0 -> +-Fmax) (Mac session). | 8de87b9 |
| `tools/build_id.py` sorted files as `Path` objects: case-insensitive on Windows, by parts on POSIX, so identical trees hashed differently. | Sort by the relative path string (Mac session). | cf9ccb2 |

Result: the stage-1 fight (2 players, stage 1, AI, `WOTM_FIXEDSTEP=1 WOTM_SEED=1234 WOTM_SETTINGS=0`) is identical on Windows and the
Mac for all 3412 compared frames, and the frame-1 call trace (`WOTM_CALLTRACE`) is identical over 12975 calls (GPR, FPR and VU0
registers). The build-id parts (native, patches, ps2recomp, recomp) are equal on both machines; only the compiler differs.

## Tools made for the hunt

- `WOTM_TRIGCHECK`, `WOTM_PADLOG` (pad stub calls of the first frames), `WOTM_CALLTRACE`/`WOTM_CALLDUMP` (Mac session): in
  `docs/passos/NATIVO.md`.
- In the coord repo `scripts/`: `framehash.ps1/.sh` (stage-1 runs), `trigcheck.ps1`, `online.ps1/.sh` (one side of a network match),
  `ramlines.py` (compare RAM dumps across machines through 64-byte line hashes, so no RAM leaves a machine), `blockdiff.py`,
  `dumpcheck.py`, `calltrace.py`, `trace_first.py`, `trace_range.py`, `fma_count.py` (FMAs per function in a dumpbin listing),
  `tree_hashes.py` (per-file hashes of a tree).
- `tools/net_lobby.py` waits for other players without the 10 s read timeout (a62b0f1), needed for lobbies across two PCs.

## Still open

- Prove more content: recorded input replayed on both machines (stage 2), 4 and 8 monsters, other stages.
- Then a clean online match Mac x Windows (the first one, with a borrowed build id, stopped on a desync at frame 31, before the
  pad and VU fixes).
- On Windows, the `WOTM_STATEDUMP=0` dump doesn't match the frame-0 hash record (frame 1 does); `run_live.ps1` always exits 0;
  `tools/live_check.ps1` aborts when it can't kill a lingering process.
- Upstream merge (50 commits behind, 13 conflicting hunks in 7 files), with Lane present; its Portuguese gets translated in the
  same go.
