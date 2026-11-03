# Bug report zip: design

Date: 2026-11-03. Branch: bug-report (on top of host-reachability). Status: agreed with Jake, written for Lane's review.

## Goal

The textures report from the friends package arrived with no data, and the TODO asks the reporter for five things (GPU, driver,
`live.err.log`, a screenshot, the mods folder). One button in the launcher writes everything but the screenshot into one zip on the
Desktop, so a report is "here is the zip" and the first reply is never "please send the log".

Launcher only (`tools/launcher`); no game change.

## What the button does

**Save bug report**, next to **Open live.err.log**. No dialog: it writes `wotm-report-<yyyyMMdd-HHmm>.zip` on the Desktop and the
status line says so, with the names of any file it could not find. The tooltip and the status line say the zip shows file paths with
the Windows user name.

### `report.txt`

One `key: value` line per fact, in this order:

- Build id (`wotm.buildid`), the package line from `BUILD.txt`, the launcher's folder and build time, the runtime folder.
- One `Mod:` line per folder in the mods folder the game reads (`ModsDir`): name, `(off)` for `_` and `.` names, file count and size.
- One `Screen:` line per monitor (size, primary).
- `Online:` the Server box, the lobby code or "no lobby", the hosted port; then the host's box lines when hosting (Router, Internet
  address, Port test) and the Windows Firewall verdict.
- From WMI (`BugReport.SystemInfo`): Windows caption and version, CPU name and cores, memory, and one line per video adapter with
  its driver version and date, adapter RAM and current mode. A query that fails becomes a line saying so; the report still comes out.
- The list of files that went in and the ones that were missing.

### Files

`live.err.log` and `live.out.log` (from `<runtime>\logs\` when that folder exists, else `<runtime>\`), `discroot\settings.ini`,
`wotm_server.log` and `WotM_Launcher.ini` next to the launcher. Files are opened for shared reading (the game or the server may still
be writing them) and a file over 4 MB goes in from the end, the newest lines. No save file, no disc file, no ISO path.

## Code

`tools/launcher/BugReport.cs`: `ReportFile { Name, Path, MaxBytes }`, `BugReport.SystemInfo()` (WMI, `System.Management`),
`BugReport.ReportText(info, included, missing)` and `BugReport.Write(zipPath, info, files)` (`System.IO.Compression`, returns the
missing names). The launcher's `SaveBugReport()` gathers the facts and the file list and calls `Write`. Both bat files gain the file
and the three references.

## Tests and docs

`LobbyTest.cs` `bug_report`: writes a zip from a small file, a long file with a 1000-byte cap and a missing file; checks the entries,
that `report.txt` carries the facts and the missing name, that the long file came in from the end, and that `SystemInfo` names a video
adapter on this PC. Docs: the friends README's "Anything else" line, `docs/en/ONLINE.md`'s desync line, and the textures item in
`TODO.md`/`docs/en/TODO.md` ask for the zip.
