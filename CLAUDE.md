# WotM: decompilation of War of the Monsters (PS2, NTSC-U, SCUS-97197)

Decompilation project for the `SCUS_971.97` executable. The end goal is a native PC port that's easy to mod.

This repo (`sightama/wotlane`) is a fork of `yanm1103/War_of_the_Monsters_recomp`. Much of the inherited code, comments and docs are in Portuguese; this fork is English-only (see "Language" below).

## Language

Everything in this fork is written in English: replies, code, comments, identifiers, log and UI strings, docs and commit messages.

Whenever you work in a file and find Portuguese in it (comments, identifiers, log/UI strings, docstrings), translate it to clear, idiomatic English as part of that change, without being asked. Write for an English-speaking developer who has never seen the Portuguese: translate the meaning, not word for word, and expand terse notes if they'd be unclear.

When translating:
- Translate meaning only. A translation must never change behavior. Keep code edits that are pure translation separate from logic changes (a separate commit when practical).
- Renaming an identifier, file or environment variable (`lote.ps1`, `WOTM_*` names) means updating every reference to it in the same change, including scripts, docs and the launcher. Search the repo first. If the name is also used upstream or by users' existing setups, ask before renaming.
- Translating code under `src/`, `include/` or `asm/` still goes through `gate.sh` (rule 2): comment-only changes can't move the ROM, but check anyway.
- Never translate game data, file-format magic strings or text the retail game itself reads or matches against.
- Expect merge conflicts when pulling from upstream into translated files. Resolve them by keeping upstream's logic and our English.
- Every merge or cherry-pick from upstream is followed by translating whatever Portuguese it brought in (new files, comments, log/UI strings, docs, commit-message text you carry over), in a separate translation commit right after the merge. Same rules as above: meaning only, and every string that code or scripts match changes together with its matches.

**Direction: port first.** The base is the game recompiled with PS2Recomp (kept outside the repo, because the generated C++ is game code) plus this repo's native layer
(`native/`: GPU drawing, sound, movies, keyboard), which already runs fights at 60 fps. The decompilation (`src/`) is the readable layer that replaces, through hooks, only what matters for modding (`Shell::Load*`, `MonsterParse`,
camera); the "guinea pig" runtime that used the equivalent functions is on hold. **Byte matching isn't required per function** (`equivalent` is enough; `matched` when it comes cheap; `gate.sh` is still mandatory). The emulated VU1/GS in the
runtime is only the reference for checking (`docs/passos/VU1CHECK.md`). The rules below still apply; where they say "matching", read "matching when that's the goal for the function".

**What to work on:** **`TODO.md`** in the repo root (the list and its order belong to the owner); the owner's bug reports take priority over it.

Read before starting: `README.md` (setup), `docs/ANALYSIS.md` (what's known about the ELF and the status conventions), `docs/PLANO.md` (architecture) and `TODO.md`. Index of `docs/passos/` ("steps"):
- `NATIVO.md`: the native layer (drawing, textures, monsters, particles, shadows, menus, the "Recompiled options" panel, icons), host hooks, watchdog, batch runs, the table of `WOTM_*` variables.
- `VU1CHECK.md`: the VU1 vs native check, corpus, the owner's photos, open bugs.
- `RUNTIME.md`: the recompiled runtime on Windows (build, `PS2X_*`, keyboard, useful addresses). `FRONTEIRA.md` ("boundary"): what of the PS2 the port no longer emulates.
- `FERRAMENTAS.md` (`tools/`, headers, difftest), `ARMADILHAS.md` (gcc 2.95 and NM build pitfalls), `ESPINHA.md` (game flow), `TESTE_PCSX2.md` (halfcpp in PCSX2).
- `MODS.md` (the `mods/` folder, PNG textures, `tools/modkit.py`), `MODELOS_3D.md` (animated monster straight from the disc), `VIEWER_WEB.md` (evaluation of the in-browser viewer; the viewer is live at https://yanm1103.github.io/wotm-model-museum/).
- Dated investigations live in `docs/investigacoes/<date>/` ("investigations"). A closed one (what it asked for is already in the code) is deleted; whatever is still open moves to `TODO.md`, and references to it become `<commit>:docs/investigacoes/...` (read with `git show`).
- `docs/FORMATOS.md`: the game's file formats (stage, monster, sound, movie, memory card save).

## Documentation: three kinds of text, each in its place
- **Reference** (`README.md`, this file, `docs/*.md`, `docs/passos/*.md`): how things are today, in the present tense. No dates, no "it used to be", no "done/fixed on", no account of how it was tested. A measured number goes in only if it's used (e.g. "9619 ticks per field"). Upstream writes these in Portuguese; in this fork they are in English (see "Language").
- **To do** (`TODO.md`): only what's open, each item short (what it is, where to start, where the context is). When it's finished the line goes away, with no "done" and no strikethrough; whatever now exists goes into the reference docs. The owner's decisions not to do something go under "Discarded".
- **Journal** (what happened, why, how it was measured and tested): the commit message and `docs/investigacoes/<date>/`.
- `python tools/doclint.py` checks the lines a commit adds plus the whole `TODO.md`; the git pre-commit hook runs it automatically (enable once per clone: `git config core.hooksPath tools/githooks`). An old line that's only edited isn't held to rules it already broke; `--all` lists what's still left to migrate.

## Port workflow (changes in `native/`)
1. Before touching anything: `tools\corpus.ps1 -Check` saves the baseline (`<runtime>\corpus\resumo_<date>.csv`, "summary"; capture the corpus once with `-Capture`). What's broken or missing comes from the inventory (`VU1CHECK.md`, "Inventário do port", the port inventory), not from memory.
2. Edit `native/*.hpp` or `native/runtime/*.inc|.cpp` (hooks and game code that run inside the runtime). Refer to addresses by name (`wotm::sym::`/`wotm::fn::` from `native/wotm_syms.h`), never as raw numbers. A new diagnostic variable goes into the table in `NATIVO.md` (`python tools/wotm_env.py` checks it).
3. `build_runtime.bat` in the repo root (configures and compiles `build_dev`, without LTCG; ninja doesn't track `native/`, the script handles that). Never the old folder's `build.bat` with LTCG.
4. Check: `tools\corpus.ps1 -Check -Baseline <baseline csv>` tells you what got worse; `run_live.bat` to see it on screen; `tools\lote.ps1` for batches of fights with a watchdog (`lote` = batch).
5. Commit without the gate (`native/`, `docs/` and `tools/` outside the build can't change the ROM). A change to a PS2Recomp seam (CMake, `main.cpp`, taps) requires regenerating `tools/runtime_patch/wotm-runtime.patch`.

## Rules that must not be broken
1. **No game files go into the repo.** `ISO/`, `disc/`, `asm/`, `assets/`, `build/`, `*.rom`, `*.elf` and `tools/cc/` are in `.gitignore`. `asm/` contains game code: never force `git add -f`, never paste game assembly into issues, PRs or documentation.
2. **The ROM must keep matching the retail SHA1.** Every commit that changes decompiled code (`src/`, `include/`, `asm/`, config read by the build) goes through `sh tools/wsl/gate.sh && git commit ...` (build + SHA1, exits with an error on failure); a commit that only touches `native/`, `docs/` or `tools/` outside the build can't change the ROM and skips the gate. Never `gate.sh | tail && git commit` or `check.sh; git commit`: the pipe hides the failure and the commit goes in with a broken build.
3. **No direct pushes to `master` unless you're the owner.** Contributors work in a fork or branch and open a PR (see `CONTRIBUTING.md`).
4. **Clearly mark what's identical and what's only equivalent.** A function that matches byte for byte gets no marking; a function that doesn't match goes inside `#ifdef NON_MATCHING` (with `INCLUDE_ASM` in the `#else`), and its comment states the score and the cause. Use `tools/nm_wrap.py`. A behavior change disguised as decomp isn't accepted; if one is necessary, flag it explicitly.
5. **Don't spend effort on hardware code** (GS/VIF/DMA, timers, SDK): it's listed in `config/hw_funcs.txt` and the port replaces it. `game/Sound` and `game/StreamingSoundManager` aren't hardware, but they're low priority.

## Environment
- Windows, with the build running in **WSL Ubuntu**: use `wsl -d Ubuntu` (the default distro is `docker-desktop`, which won't work). Venv: `~/.venvs/wotm/bin/python`. Repo root inside WSL: `/mnt/c/Users/<you>/WoTM`.
- Compiler: ee-gcc 2.95.2 (SN ProDG), downloaded into `tools/cc/` by `tools/setup_toolchain.sh`. Requires your own ISO in `ISO/`, extracted to `disc/`.
- `.sh` scripts need LF line endings. Writing them with Python on Windows produces CRLF and WSL's `sh` breaks (`sed -i 's/\r$//'` fixes it).
- `config/status.csv` (the state of each function) is generated by `python3 tools/progress.py`; only the owner commits it. **Contributors don't commit this file**, so it doesn't cause conflicts in every PR. The README has no progress block.
- When writing files that contain regexes or `\n`, use the Edit/Write tools. A Python heredoc turns `\n` into a real line break.
- **Never run Python inline** (`python3 - <<EOF`, `python -c "...\..."`): Windows paths (`C:\Users\...`) and `\n` break. A hook (`.claude/hooks/block_python_heredoc.py`, enabled in `.claude/settings.json`) rejects these commands. Save the script with Write (to a scratch folder) and run `python file.py`; use Edit/Write to change repo files.

## Per-function workflow
1. Read `asm/nonmatchings/<tu>/<function>.s`. For more than ~30 instructions, `sh tools/m2c.sh <tu> <function>` gives a draft. Never trust it without reading the asm (the arguments it shows are register noise). Replace `unkNNN` with named fields from the headers.
2. Convert the TU to C++ if it isn't already: `sh tools/wsl/convert_tus.sh <Tu>` (one at a time).
3. Write natural C++, in the source's natural order (it often matches right away). Score with `sh tools/wsl/scoreall.sh <TU>`; if it doesn't match, `tools/nm_wrap.py <file> <symbol> "<note>" [Class::method]`. Only fine-tune when it's cheap (`tools/wsl/variants.py`, `tools/wsl/permute.py`).
4. Verify the whole TU in one command: `~/.venvs/wotm/bin/python tools/verify_tu.py game/<TU>` (score, difftest, call sequence, layout and consumed return values, in one table per function; `--no-nm` skips the ELF for the equivalent functions and takes seconds instead of minutes). m2c drafts for the whole queue: `sh tools/m2c_batch.sh` (into `build/m2c/`).
5. Run `gate.sh`, then `python3 tools/progress.py`, then commit.

## Known pitfalls (bugs that have already happened)
An "untuned" score can hide a semantic error. After writing an equivalent function, check:
- **Return value used by the caller.** A `void` where the retail code returns `$v0` (e.g. `addInteractive`: the slot index is the hat's id). `tools/nm_retvals.py`.
- **Call sequence.** `tools/nm_calls.py`.
- **Struct size and stride.** Add a `static_assert` with the retail stride (e.g. `PathNode` is 0x40, not 0x30). An empty base class takes up one byte (`AiBrain`).
- **Delay slots.** An inverted compare hidden in the delay slot (e.g. `Monster::isIdle`) and a wrong branch target (e.g. `AiGrappleAttack`, heavyPunch vs toss). Read the asm all the way to the end.
- **Order and helpers.** When replacing `INCLUDE_ASM` with C++, keep functions in their original address order (each definition where its `INCLUDE_ASM` was, never grouped together). `static`/`static inline` helpers go inside `#ifdef NON_MATCHING`: ee-gcc 2.95 emits unused helpers and shifts the ROM.
- **Strings and rodata.** Moving strings from `INCLUDE_ASM` into C changes the rodata padding (add `.word 0` in asm in `.rodata`). `switch` jump tables in C need the same padding (which is why a function with a `switch` stays in `NON_MATCHING` even when it matches: the table comes from the retail rodata). Virtual classes: keep the vtable, ctor and `__tf` as `INCLUDE_ASM`; `tools/place_data.py` places data by address.
- **Shared headers.** Moving a class into a header can change `sizeof` and shift fields in partial structs. Before touching one: `sh tools/wsl/scoreall.sh <all TUs in src/game> > ~/base.txt`; afterwards compare with `diff`.
- **Compiler tricks.** `__asm__("#SNFIX_SMALL sym")` marks a global as gp-relative; `register float x __asm__("$f2")` pins an FPR register; an `int` vs `void` return on the callee changes register allocation; arguments are evaluated left to right (hoisting a call into a local moves an `addiu`); `sqrt.s` needs `.word 0x46040104` (in non-matching C++ use `eeSqrtf` from `include/vecmath.h`: gas encodes `sqrtf` in the MIPS32 form and the EE reads `$f0`).

## Order of attack
Decompile top-down along the real execution path, not by filtering for "small functions": `main` → `Shell::LoadLevelFiles/LoadLevelDB/LoadMonstersDB` → `dbInitDb`/`dbsRelocate*`/`file*` → `InitPlayers`/`AddMonster`/`MonsterParse` → the `rtMain` loop (hierarchy/animation/drawing) → the logic in `TheGame::Update`. These loaders also document the file formats. For scope by reachability: `python3 tools/callgraph.py <roots> --no-libs` (writes `config/callgraph.csv`; copy and restore it if you don't want it overwritten; it only follows `jal`, so it's a lower bound).

## Behavior verification
No unit tests, by project decision. Verification is done with `tools/difftest.py` / `difftest_all.py` (retail vs our function in a MIPS emulator, same random state) and, once something is running, comparison against PCSX2. To debug a symptom: pause PCSX2, dump the EE's RAM and recompute the retail logic from the dump (`tools/ramdiff.py`). If an ELF variant crashes with "executing data", compare the EE RAM against the ELF before suspecting the decompiled function (this has happened: it was relocated code overwritten by DMA).

**Dumps and snapshots are disposable.** `.vu1snap` files (~40 MB each), `ram_*.bin`/`vram_*.bin` (36 MB), watchdog dumps and `lote/` folders stay out of the repo and fill up the disk. Once a fix is validated, delete the snapshots and dumps that prompted it (`snaps/`, `discroot/ram_*`, `lote/<date>/`); keep only the ones that still show an open problem or that you deliberately named as a regression corpus (`tools/vu1cmp.ps1 <folder>`). Never leave `WOTM_VU1CHECK_SNAP` on during a batch run.
