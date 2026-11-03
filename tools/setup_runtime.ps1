# ABOUTME: Sets up everything the port needs from your ISO in one command: extracted disc, patched PS2Recomp, translated game
# ABOUTME: (recomp\retail), the runtime folder (runtime\) with its scripts, and the first wotm.exe build. Safe to rerun: skips what exists.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\setup_runtime.ps1 [-Iso <path to the .iso>] [-SkipBuild] [-AnyCompiler]
#   Without -Iso it uses the first .iso in ISO\. Requires: git, Python 3, CMake 3.24 or newer and Visual Studio 2022 (or Build Tools)
#   with C++ compiler 14.40 or newer (14.32 hits an internal compiler error on the translated code). None of the game goes into the
#   repo: disc\, recomp\ and runtime\ are in .gitignore.
param([string]$Iso = '', [switch]$SkipBuild, [switch]$AnyCompiler)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$rt = Join-Path $repo 'runtime'
$elfSha1 = '1ed7f8457436a9170d8f85386e267369c7fcdd4f'   # SCUS_971.97 from the NTSC-U disc (SCUS-97197)
$ps2recompCommit = '2c5fbb9389e11dd95693385969490c9e8e6f57b4'   # base of tools\runtime_patch\wotm-runtime.patch

function Step([string]$m) { Write-Host "== $m" -ForegroundColor Cyan }
function Fail([string]$m) { Write-Host $m -ForegroundColor Red; exit 1 }
# Native command whose error output is expected (probes): run through cmd, stderr discarded. In PowerShell 5.1 with Stop, any stderr
# line from a program becomes a fatal error. Returns the exit code.
function Probe([string]$cmdLine) { & $env:ComSpec /c "$cmdLine >nul 2>nul"; return $LASTEXITCODE }

# --- tools
Step 'tools'
foreach ($t in 'git', 'python') { if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { Fail "$t is missing from PATH" } }
$cmake = @((Get-Command cmake -ErrorAction SilentlyContinue).Source, 'C:\Program Files\CMake\bin\cmake.exe') | Where-Object { $_ -and (Test-Path $_) } |
    Where-Object { [version](((& $_ --version)[0] -split ' ')[2] -replace '-.*$', '') -ge [version]'3.24' } | Select-Object -First 1
if (-not $cmake) { Fail 'CMake 3.24 or newer is missing (winget install Kitware.CMake)' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail 'Visual Studio 2022 / Build Tools with C++ is missing (winget install Microsoft.VisualStudio.2022.BuildTools)' }
# The pinned compiler is MSVC 14.44 (VS 2022 17.14), the one online matches are tested with. Game code builds with strict FP and
# the build id leaves the compiler out; if another compiler simulates differently, the server's per-frame state hash stops the match.
# -AnyCompiler accepts another 14.40+ (untested online).
$pinnedMsvc = '14.44'
$vcvars = $null; $vcvarsArgs = ''
foreach ($vs in (& $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)) {
    if (-not (Test-Path "$vs\VC\Auxiliary\Build\vcvars64.bat")) { continue }
    $tools = @(Get-ChildItem "$vs\VC\Tools\MSVC" -ErrorAction SilentlyContinue | Sort-Object { [version]$_.Name })
    if ($tools | Where-Object { $_.Name -like "$pinnedMsvc.*" }) { $vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"; $vcvarsArgs = "-vcvars_ver=$pinnedMsvc"; break }
    if ($AnyCompiler -and -not $vcvars -and $tools -and [version]$tools[-1].Name -ge [version]'14.40') { $vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat" }
}
if (-not $vcvars) {
    Fail ("MSVC compiler $pinnedMsvc is missing. In the Visual Studio Installer: Modify > Individual components > " +
          "'MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44-17.14)' (every PC in an online match needs it). " +
          'To play offline only with another 14.40+ compiler: -AnyCompiler')
}
if (-not $vcvarsArgs) { Write-Host "WARNING: MSVC $pinnedMsvc not found; online play with another compiler is untested" -ForegroundColor Yellow }
Write-Host "cmake: $cmake`nvcvars: $vcvars $vcvarsArgs"

# --- disc
Step 'disc'
if (-not $Iso) { $Iso = (Get-ChildItem (Join-Path $repo 'ISO') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
if (-not $Iso -or -not (Test-Path $Iso)) { Fail 'no .iso: put yours in ISO\ or pass -Iso' }
$disc = Join-Path $repo 'disc'
if (-not (Test-Path (Join-Path $disc 'SCUS_971.97'))) {
    $img = Mount-DiskImage -ImagePath $Iso -Access ReadOnly -PassThru
    try {
        $letter = ($img | Get-Volume).DriveLetter
        New-Item -ItemType Directory -Force $disc | Out-Null
        robocopy "${letter}:\" $disc /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { Fail "robocopy failed ($LASTEXITCODE)" }
    } finally { Dismount-DiskImage -ImagePath $Iso | Out-Null }
    Get-ChildItem $disc -Recurse -File | ForEach-Object { $_.IsReadOnly = $false }
}
$sha = (Get-FileHash (Join-Path $disc 'SCUS_971.97') -Algorithm SHA1).Hash.ToLower()
if ($sha -ne $elfSha1) { Fail "SCUS_971.97 has SHA1 ${sha}: the port only supports the NTSC-U disc (SCUS-97197, $elfSha1)" }

# --- PS2Recomp with the repo's patches
Step 'PS2Recomp'
New-Item -ItemType Directory -Force $rt | Out-Null
$pr = Join-Path $rt 'PS2Recomp'
if (-not (Test-Path (Join-Path $pr '.git'))) {
    git clone -q https://github.com/ran-j/PS2Recomp.git $pr
    git -c advice.detachedHead=false -C $pr checkout -q $ps2recompCommit
}
foreach ($p in 'wotm-runtime.patch', 'ps2recomp-fpu-sqrt.patch') {
    $patch = Join-Path $repo "tools\runtime_patch\$p"
    if ((Probe "git -C `"$pr`" apply --whitespace=nowarn --reverse --check `"$patch`"") -eq 0) { Write-Host "$p already applied"; continue }
    if ((Probe "git -C `"$pr`" apply --whitespace=nowarn `"$patch`"") -ne 0) { Fail "$p does not apply to $pr (did the clone change?)" }
    Write-Host "$p applied"
}

# --- runtime folder scripts (from the templates in tools\runtime_template)
Step 'runtime scripts'
$repoFwd = $repo -replace '\\', '/'
$cmakeBin = Split-Path $cmake -Parent
foreach ($f in 'configure_dev.bat', 'build_dev.bat', 'build_tools.bat', 'run_live.ps1') {
    $text = Get-Content (Join-Path $PSScriptRoot "runtime_template\$f") -Raw
    $text = $text.Replace('@VCVARS_ARGS@', $vcvarsArgs).Replace('@VCVARS@', $vcvars).Replace('@CMAKE_BIN@', $cmakeBin).Replace('@REPO@', $repoFwd).Replace('@ISO@', (Resolve-Path $Iso).Path.Replace("'", "''"))
    Set-Content (Join-Path $rt $f) $text -NoNewline -Encoding ascii
}
Set-Content (Join-Path $repo '.wotm_recomp_dir') $rt -Encoding ascii
New-Item -ItemType Directory -Force (Join-Path $rt 'discroot') | Out-Null
$elfDst = Join-Path $rt 'discroot\SCUS_971.97'   # the rest of the disc comes from the ISO (PS2X_CD_IMAGE)
# copy only if different: the target may be a hardlink to disc\SCUS_971.97 itself (copying onto itself fails)
if (-not (Test-Path $elfDst) -or (Get-FileHash $elfDst -Algorithm SHA1).Hash.ToLower() -ne $elfSha1) {
    Copy-Item (Join-Path $disc 'SCUS_971.97') $elfDst -Force
}
& (Join-Path $PSScriptRoot 'sync_mods.ps1') -Runtime $rt -Repo $repo   # repo mods (mods\<mod>\) into discroot\mods\

# --- translated game (game code: local only, recomp\ is in .gitignore)
Step 'translated game'
$retail = Join-Path $repo 'recomp\retail'
if (-not (Test-Path (Join-Path $retail 'register_functions.cpp'))) {
    & $env:ComSpec /c "`"$rt\build_tools.bat`" 2>&1" | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { Fail 'building the analyzer/recompiler failed' }
    $work = Join-Path $repo 'recomp\work'
    New-Item -ItemType Directory -Force $work | Out-Null
    Copy-Item (Join-Path $disc 'SCUS_971.97') (Join-Path $work 'game.elf') -Force
    Push-Location $work
    try {
        & "$rt\build_tools\ps2xAnalyzer\ps2_analyzer.exe" game.elf config.toml | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail 'ps2_analyzer failed' }
        (Get-Content config.toml) -replace '^output = .*', 'output = "../retail/"' | Set-Content config.toml -Encoding ascii
        & "$rt\build_tools\ps2xRecomp\ps2_recomp.exe" config.toml | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail 'ps2_recomp failed' }
    } finally { Pop-Location }
}
$cpp = (Get-ChildItem $retail -Filter *.cpp).Count
Write-Host "$cpp translated files"

# --- build
if (-not $SkipBuild) {
    Step 'configure and build (the first build takes a few minutes)'
    if (-not (Test-Path (Join-Path $rt 'build_dev\build.ninja'))) {
        & $env:ComSpec /c "`"$rt\configure_dev.bat`" 2>&1" | Select-Object -Last 2
        if (-not (Test-Path (Join-Path $rt 'build_dev\build.ninja'))) { Fail 'configure_dev.bat failed' }
    }
    & $env:ComSpec /c "`"$repo\build_runtime.bat`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Fail "build failed: see $rt\logs\build_dev.log" }
}
Step 'done'
Write-Host "To play: run_live.bat -Auto 0   (or tools\launcher\build.bat for the launcher)"
