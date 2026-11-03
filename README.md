<img width="2113" height="744" alt="War of The Monsters recompiled LOGO" src="https://github.com/user-attachments/assets/6160bebd-48f0-47b5-bd28-f74d23c662fd" />

# War of the Monsters Recompiled!

A decompilation and recompilation project for **War of the Monsters** (PS2, NTSC-U, SCUS-97197), aiming at an **easy-to-mod native PC port**.

You need your own copy of the game. No game files are, or ever will be, in this repository.

## About this fork
This repository ([sightama/wotlane](https://github.com/sightama/wotlane)) is a fork of
[yanm1103/War_of_the_Monsters_recomp](https://github.com/yanm1103/War_of_the_Monsters_recomp) and the primary home of our work, mainly online play with friends on separate
PCs. We merge upstream changes wherever they make sense. Our changes may be offered to upstream later as pull requests, as a courtesy, if the upstream owner wants them.

## Status
The game runs on Windows at 60 fps with native rendering: stages, monsters, particles, shadows, fog, menu text, HUD, screen fades and Hor+ widescreen. Fights are playable with the
keyboard AND gamepad, and sound and FMVs play natively. Geometry matches the PS2's VU1 output on more than 99% of triangles. Lighting (directional, point lights such as fire) follows the VU1 as well.

**Not an emulator.** The EE (the PS2's CPU) code is statically recompiled to C++ and runs natively; VU0 code is translated too (inline instructions by PS2Recomp, microprograms by
`tools/vu0native.py`). The game executable (`wotm.exe`) has no emulated VU1, VIF0, VIF1, GS or IOP; those only exist in a separate test executable, the VU1/GS as the reference for the renderer check.
What is still PS2-specific in the game: [docs/passos/FRONTEIRA.md](docs/passos/FRONTEIRA.md).

The port inventory ([docs/passos/VU1CHECK.md](docs/passos/VU1CHECK.md)) is finished; what comes next is in [TODO.md](TODO.md).

8 Monsters brawling: [https://www.youtube.com/watch?v=IvLLuSYORPc](https://www.youtube.com/watch?v=IvLLuSYORPc).

<img width="912" height="822" alt="image" src="https://github.com/user-attachments/assets/77483682-f8e2-470a-95aa-06aabe857b6a" />

<img width="2560" height="1440" alt="image" src="https://github.com/user-attachments/assets/8f837f1f-c5a8-49fe-8ad0-30bfd13bdf84" />

<img width="2560" height="1440" alt="image" src="https://github.com/user-attachments/assets/4583be43-878b-48df-a0e5-30b99702bad9" />

<img width="2560" height="1440" alt="image" src="https://github.com/user-attachments/assets/ddb512fd-ebf0-46d8-90bd-cc40ee9a4334" />

<img width="2560" height="1440" alt="image" src="https://github.com/user-attachments/assets/562f7135-1780-4582-b8d2-fed4b5ca9e85" />

Screenshots and older videos, by date: [docs/screenshots](docs/screenshots).

## How it works
The port comes first; the decompilation only covers what modders need.

1. **Static recompilation as the base.** [PS2Recomp](https://github.com/ran-j/PS2Recomp) translates the whole executable (5,382 functions, zero decoding failures, VU0 included) into
   C++ that runs on its own runtime. That C++ is a literal, function-by-function translation over an emulated PS2 RAM. It is game code, so it **never goes into this repository**: everyone
   generates it locally from their own disc.
2. **Native layer** ([native/](native/)). Instead of emulating the VU1 and GS, we cut at the engine boundary (scene hierarchy, packets, display lists, particles; see
   `config/hw_boundary.csv`) and draw with the PC's GPU (raylib). Sound, FMVs and keyboard input are native too. The code that runs inside the runtime as hooks lives in
   [native/runtime/](native/runtime/). The emulated VU1 is kept only in the test executable, as a reference to check the native renderer against.
3. **Readable decompilation** ([src/](src/), [include/](include/)). Game logic, AI, monsters, stages and file formats become readable C++ that can replace the recompiled version through a hook.
   A function is either `matched` (byte-identical to the retail code) or `equivalent` (same behavior, not byte-identical). Byte matching is optional per function, but the full ROM build
   must still reproduce the retail SHA1.

Architecture, and what lives in the repo versus what is generated locally: [docs/PLANO.md](docs/PLANO.md). What is known about the executable: [docs/ANALYSIS.md](docs/ANALYSIS.md).
File formats: [docs/FORMATOS.md](docs/FORMATOS.md).

## Building the port (macOS and Linux)
A native build for the machine's own CPU (Apple Silicon included, through sse2neon), with the same steps as on Windows:
1. Install the tools. macOS: `brew install cmake ninja pkgconf ffmpeg sevenzip` plus the Xcode Command Line Tools. Linux: git, Python 3,
   CMake 3.24+, Ninja, gcc or clang, pkg-config, the FFmpeg development packages and 7-Zip.
2. Put your ISO in `ISO/` and run `sh tools/setup_runtime.sh`. It extracts the disc, sets up PS2Recomp in `runtime/`, generates the
   recompiled code and runs the first build.
3. `sh tools/build_runtime.sh` rebuilds after changes; `sh runtime/run_live.sh --auto 0` runs the game (log in `runtime/live.err.log`).
4. `sh tools/vu0bench.sh` checks that the native VU0 code gives bit-identical results to the interpreter on this machine.

Not available yet outside Windows: rumble, the launcher and a packaged release.

## Building the decompilation (WSL/Ubuntu)
```sh
sudo apt install binutils-mips-linux-gnu ninja-build python3-venv
python3 -m venv ~/.venvs/wotm && ~/.venvs/wotm/bin/pip install "splat64[mips]" pyelftools
sh tools/setup_toolchain.sh        # downloads ee-gcc 2.95.2 (SN ProDG) into tools/cc/
```
1. Put your ISO in `ISO/` and extract it to `disc/` (for example `7z x -odisc ISO/*.iso`).
2. `~/.venvs/wotm/bin/python configure.py --split` builds the ROM and symbols, runs splat and writes `build.ninja`.
3. `ninja` assembles, links and checks the SHA1 against the original executable (`build/SCUS_971.97.rom: OK`).

Before committing changes to decompiled code, run `sh tools/wsl/gate.sh` (build + SHA1 check).

### Notes for a fresh clone
- `configure.py` needs Python 3.10 or newer in the venv. On Ubuntu 20.04, whose `python3` is 3.8 and whose apt has no 3.10, `uv python install 3.12` gives a user-space
  Python to build the venv from.
- `tools/cc.sh` runs the compiler (a Win32 binary) through `wine`, which needs the i386 architecture enabled **before** installing it:
  ```sh
  sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install wine wine32:i386
  ```
- On a fresh clone, `configure.py --split` may skip generating the data `.s` files (vtables such as `_vt$12PointToolKit.s`) and `ninja` then fails on a missing file (seen with splat
  0.41.1 and 0.50.0). Workaround: move `src/` out of the repo, run `--split`, then put `src/` back. The cause has not been investigated yet.
- `tools/difftest.py` needs `unicorn` and `pyelftools` in the venv and runs in WSL.
- When the checksum fails, `tools/symdiff.py` lists misplaced symbols and `tools/romdiff.py` shows the words that differ.

## Building and running the port (Windows)
- Clone with submodules (`git clone --recurse-submodules`): PS2Recomp is the `third_party/PS2Recomp` submodule (branch `wotm` of the `yanm1103/PS2Recomp` fork, which carries the
  `SQRT.S`/`RSQRT.S` fix from [PR #277](https://github.com/ran-j/PS2Recomp/pull/277)).
- **One command from your ISO:** put the ISO in `ISO/` (or pass `-Iso <path>`) and run `powershell -ExecutionPolicy Bypass -File tools\setup_runtime.ps1`. It extracts the disc to
  `disc/`, clones PS2Recomp at the base of `tools/runtime_patch/wotm-runtime.patch` into `runtime/` (git-ignored), applies that patch and the recompiler's `SQRT.S`/`RSQRT.S` fix
  (`tools/runtime_patch/ps2recomp-fpu-sqrt.patch`), generates the recompiled code into `recomp/retail` (git-ignored), writes the runtime folder scripts from
  `tools/runtime_template/` and runs the first build. Needs git, Python 3, CMake 3.24+ and MSVC 14.40+ (VS 2022 17.10+ or Build Tools; 14.32 hits an internal compiler error on
  the generated code). Re-running it skips what already exists.
- `build_runtime.bat` builds the runtime with the native layer (`build_dev`, fast link without LTCG): `wotm.exe` (the game) and `ps2EntryRunner.exe` (test build with the
  emulated VU1/GS and the renderer check).
- `run_live.bat -Auto 0` runs the game to play with the keyboard; `tools/launcher/` is a small launcher with options such as starting a match directly (`WOTM_MATCH`) and cheats.
- **Online play (this fork, 2 to 8 players, each on their own PC):** deterministic lockstep through a small server (`native/net`, `wotm_server`, no game
  code). Setup, hosting, router and lobby steps: [docs/ONLINE.md](docs/ONLINE.md).
- `mods/` holds the project's own mods (the Goliath Prime custom monster, the "Recompiled" logo): JSON and original art only. Every launch mirrors them into
  `<runtime>\discroot\mods\` (`tools/sync_mods.ps1`), where the game reads mods from; your own mods live there too. See [docs/passos/MODS.md](docs/passos/MODS.md).

Keyboard controls, environment variables and runtime details: [docs/passos/RUNTIME.md](docs/passos/RUNTIME.md) and [docs/passos/NATIVO.md](docs/passos/NATIVO.md).

## Contributing
See [CONTRIBUTING.md](CONTRIBUTING.md). The rules in [CLAUDE.md](CLAUDE.md) apply to humans too: no game files or game assembly in the repo, issues or PRs, and the ROM must keep matching
the retail SHA1. The documentation is in English; some file names (`docs/passos/` "steps", `NATIVO.md`, `GLOSSARIO.md`) are still the inherited Portuguese ones.

## License
The code in this repository is [GPL-3.0](LICENSE). It only covers what is here (decompilation, tools, native layer) and grants no rights over the game, which still belongs to its owners.
