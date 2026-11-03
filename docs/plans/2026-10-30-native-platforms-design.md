# Native builds on every platform: design

Goal: the port builds and runs natively on each OS/arch, with a real per-platform binary using the GPU and CPU directly. No Wine,
Rosetta or emulator wrapper. macOS on Apple Silicon comes first, the Windows build must behave exactly as it does today, Linux
comes next, and phones come eventually.

## Decisions (made with batman)

| Topic | Decision | Why |
|---|---|---|
| Base branch | `wotlane/native-platforms`, cut from `upstream/master` (`7df1f85`) | Port commits stay upstreamable as-is. The first-run/ISO picker/data-dir code (`native/runtime/wotm_standalone.cpp`) and `tools/release.ps1` exist only upstream. Merging `wotlane/4p` is a separate task (7 conflicts, including the runtime `.patch`). |
| Game ISO | Copied from `~/Downloads` into `ISO/`, extracted to `disc/` | `disc/SCUS_971.97` SHA1 `1ed7f8457436a9170d8f85386e267369c7fcdd4f`; the ROM derived from it matches `SCUS_971.97.sha1`. |
| ee-gcc / decomp build | Deferred | This work changes no `src/`/`include/`/`asm/`, so `gate.sh` is never needed. If that changes, stop and choose (OrbStack VM, Wine+Rosetta, or Windows). |
| Rendering backend | None now; keep raylib on GL 3.3 | Drawing already goes through raylib's `rlgl` (about 300 call sites, about 80% of them in `wotm_fx/native/model/preview.hpp`). Raw GL is only `glReadPixels`, a PBO and timer queries. GLSL is `#version 330`, which macOS's GL 4.1 core profile supports. The later route to Metal and phones is raylib's GLES3 build, possibly on ANGLE (Metal/Vulkan), not a renderer of our own. Re-evaluate with measured frame times after milestone 3. |

## What's already known

- **Upstream's Linux probe** (`git show upstream/master:docs/investigacoes/2026-10-30/linux.md`): the translated game compiles with gcc 15
  unchanged. `native/` needs 7 small fixes plus 1 signed-overflow UB fix. That got it to the menu under WSLg.
- **The runtime is not the submodule.** `tools/setup_runtime.ps1` clones `ran-j/PS2Recomp` at `2c5fbb9389e11dd95693385969490c9e8e6f57b4` into
  `runtime/PS2Recomp` and applies `tools/runtime_patch/wotm-runtime.patch` and `ps2recomp-fpu-sqrt.patch`. The two patches don't overlap
  (the sqrt patch only touches `ps2xRecomp/`). `third_party/PS2Recomp` and `wotm-fmv.patch` aren't used by setup.
- **arm64 SIMD coverage** (scanned against sse2neon v1.9.1, the tag PS2Recomp's CMake fetches on ARM): every `_mm_*` intrinsic used by the
  patched runtime and by `native/` exists in sse2neon. There are two exceptions:
  - The `_mm_custom_*` helpers are defined inline in `ps2_runtime_macros.h`, so they're fine.
  - **Ten AVX double-precision intrinsics** (`_mm256_*_pd`) in `native/runtime/wotm_vu0_native.cpp` (`classify4`, `fin4`, `ADD4`/`SUB4`/`MUL4`
    and madd/msub). sse2neon has no AVX, so these need a non-x86 path.
  - Two direct includes break on ARM: `<immintrin.h>` (`wotm_vu0_native.cpp:13`) and `<smmintrin.h>` (`wotm_native.hpp:27`).
- **FP fidelity on arm64:**
  - The VU sources already compile with `-ffp-contract=off -fno-fast-math` on non-MSVC (`WOTM_VU_STRICT_SOURCES`), which matters
    because arm64 always has FMA.
  - sse2neon's `_mm_setcsr` maps the rounding bits (round to zero) and FTZ/DAZ to FPCR. ARM has one flag where x86 has FTZ and DAZ.
- **Dependencies:**
  - raylib comes from FetchContent pinned to `5.5`, the same version as Windows. Homebrew's raylib 6.2 is not used.
  - On non-Windows, FFmpeg is found through `pkg-config` (`libavcodec libavformat libavutil libswresample libswscale`). This needs
    `brew install ffmpeg pkgconf`.
  - This Mac has CMake 4.4. If raylib 5.5's CMake declares a minimum below 3.5, configure fails, and the fix is
    `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` in our configure step. Not yet observed; we'll find out on first configure.
- **Shell environment:** the agent's non-login shell doesn't have `/opt/homebrew/bin` on `PATH`. The scripts must find Homebrew tools
  themselves and not assume an interactive shell.

## Architecture

One non-Windows path (POSIX, clang or gcc, CMake + Ninja) with small per-OS pieces. No separate Mac and Linux forks.

- **Compile-time splits.** In C++, `#ifdef _WIN32` keeps every Windows line as it is today. `#elif defined(__APPLE__)` / `#else`
  (Linux) are added only where the OSes differ (file picker, data dir, crash symbolization). For CPU, `#if defined(__x86_64__) ||
  defined(_M_X64)` against `USE_SSE2NEON`, which PS2Recomp defines on ARM. In CMake, `if(MSVC)` blocks stay byte-identical;
  non-MSVC flags go in the existing `else()` branches, with x86-only flags guarded by `NOT PS2X_IS_ARM_TARGET`.
- **Scripts.** POSIX `sh` scripts sit beside the PowerShell ones and use the same names and steps: `tools/setup_runtime.sh`,
  `tools/build_runtime.sh`, `tools/sync_mods.sh`, `tools/runtime_template/run_live.sh`. The `.ps1`/`.bat` files are left alone. They write
  to the same places (`runtime/`, `recomp/retail/`, `<build>/wotm_gen/`), so the two flows stay recognizably the same.
- **Generated files** stay out of git, as today: `recomp/retail/`, `runtime/`, `<build>/wotm_gen/` (VU0 `.gen.inc`, version header and,
  new, the embedded resources header).
- **Resources (Linux report item 5).** On Windows, `wotm.rc` keeps doing what it does. On other OSes, `tools/build_runtime.sh` turns
  the same files into `<build>/wotm_gen/wotm_resources.gen.h` (byte arrays), and each loader picks the resource or the array
  under `#ifdef _WIN32`. CMake doesn't change for this.
- **PS2Recomp seams.** Changes go into `runtime/PS2Recomp` and are folded back into `tools/runtime_patch/wotm-runtime.patch` by a
  new `tools/regen_runtime_patch.sh`. It diffs exactly the patch's own file list plus any new files and leaves out the sqrt patch's files.
- **Platform services** (milestone 4) live next to their Windows versions in `wotm_standalone.cpp` / `wotm_live.inc`:
  - ISO picker: `osascript` `choose file` on macOS (no Objective-C needed), `zenity`/`kdialog` on Linux, otherwise a CLI argument.
  - Data dir: `~/Library/Application Support/WotM` on macOS, `$XDG_DATA_HOME/WotM` on Linux.
  - Crash handler: `sigaction` + `backtrace()`, symbolized after the fact with `atos` (macOS) or `addr2line` (Linux).
  - Message boxes: macOS uses `osascript` `display dialog`; Linux uses stderr (what `box()` already does).
  - Rumble (GameController.framework / evdev) is last.

## Verification (and where it leaves a gap)

This project deliberately has no unit tests, and its rule takes priority here over the global TDD rule. Each milestone is verified
the project's way:
- Build output.
- `file` on the binary, which must say Mach-O arm64.
- The game on screen, using the F3 FPS counter, `live.err.log`, and the `[native]`/`[watchdog]`/`[match]` log lines.
- `WOTM_MATCH`/`WOTM_AIVSAI` to reach a fight without the pad.
- Later, `corpus`/`lote`-style batches ported to `sh`.

Gaps that leave:
- **Nothing automatically checks that x86 and arm64 compute the same.** The `_mm256_*_pd` replacement and sse2neon's emulated
  semantics (rounding, NaN handling in min/max) could drift. Mitigation: `vu0bench`-style differential runs. `PS2X_VU0_VERIFY=1`
  compares native VU0 against the interpreter, and it runs on the Mac too.
- **Windows can't be built here.** "Windows unchanged" is enforced by construction (only `#ifdef`/`else()` additions, MSVC blocks
  untouched, checked by reviewing the diff) and by batman running `build_runtime.bat` and a fight on the Windows machine at milestone 7.
- **Linux isn't on this Mac.** Milestone 5 needs a Linux box, an OrbStack VM, or WSL on the Windows machine.

## Milestones

1. The runtime generates and compiles on macOS arm64 (`wotm` and `ps2EntryRunner` link and are Mach-O arm64).
2. It boots to the menu with FMVs and sound (`run_live.sh`).
3. A full fight runs at 60 fps (F3 counter; `WOTM_MATCH` + `WOTM_AIVSAI` for a long, unattended fight).
4. First run (no arguments), data dir, crash traces, message boxes.
5. The `sh` scripts verified on Linux too (same scripts, Linux branches).
6. A `.app` bundle (FFmpeg dylibs inside, `install_name_tool`/rpath, ad-hoc `codesign`), and a Linux tarball.
7. Windows still builds and plays identically (batman, on the Windows machine).

Rumble and the GLES/ANGLE evaluation come after 7.

## Out of scope here

- Merging `wotlane/4p`.
- Any `src/` change.
- A second renderer.
- Porting `lote.ps1`/`corpus.ps1`. That happens only if milestone 3 needs batches, and the plan says so where it does.
- PRs to yanm1103's repos. Commits are kept small and upstreamable, but nothing gets opened without asking batman first.
