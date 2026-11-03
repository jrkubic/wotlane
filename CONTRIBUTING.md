# Contributing

Thanks for helping! The project is a decompilation whose goal is a native PC port that is easy to mod; read [CLAUDE.md](CLAUDE.md) (it works for humans too) and the [README](README.md) for the setup.

## What to prioritize
- **Byte match is not required.** An `equivalent` function (readable C++, behavior checked) is worth as much as a `matched` one for the goal. If it matches cheaply, great; don't spend hours tuning.
- **Choose by what matters for modding**, not by size: game logic, AI, monsters, stages, animation and file formats. The queue is in `config/boot_coverage_asm.csv`
  (functions still in asm that the game calls from boot to the menus). Leave memory card (`McPage`/`McFile`), `input`, `vi`, `ps`, `sce/*` and `lib989snd` for last.
- **Check the behavior**: `tools/difftest.py` for functions without VU0; `tools/recomp_oracle` (see its README) for those that use VU0 or `min/max/madd`. Don't use `sqrtf` in new code: use `eeSqrtf` (`include/vecmath.h`).

## License
By contributing, you agree that your contribution is distributed under the project's [GPL-3.0](LICENSE).

## Before you start
- You need **your own copy** of the game (NTSC-U, SCUS-97197). Nothing from the game goes into the repo: no ISO, no `asm/`, no assembly snippets in issues/PRs.
- Do the README setup and confirm that `ninja` finishes with `build/SCUS_971.97.rom: OK` before touching anything.
- Agree with the owner on which area (TU) you will take, so we don't work on the same one. Shared headers (`include/game/game.h`, `include/engine.h`) are the most likely conflict point: give notice before changing them.

## Workflow
1. Fork the repo (or branch, if you have write access) from an up-to-date `master`: `git switch -c <tu>-<topic>`.
2. One PR per TU or per small group of related functions.
3. Before each commit: `sh tools/wsl/gate.sh` must end in `ROM OK`.
4. **Don't commit** `config/status.csv`: it is generated (`tools/progress.py`) and causes conflicts in every PR. The owner regenerates it when merging the PR. Avoid `git commit -a`.
5. Open the PR against `master`, filling in the template. There is no CI (it can't be built without the ROM), so the `gate.sh` output in the PR is the proof. Only the owner and `LostTwilightG` merge into `master`.
6. Keep the branch up to date with `git rebase origin/master`.

## Using Claude Code
OK and encouraged. `CLAUDE.md` already carries the rules. Remind your Claude to run the gate before every commit.
