# ABOUTME: WOTM_NOINPUT mouse test: records a fight and replays it, both with WOTM_NOINPUT=1 while real mouse motion is injected
# ABOUTME: (Win32 SendInput); the mouse must not reach the game (no cursor capture) and the per-frame state must be identical.
#
# Usage: powershell -File tools\mouse_check.ps1 [-Match 3,1,1] [-Secs 50]
#   Players on the pad (PS2X_PAD script, as determinism_check.ps1 -Replay), so the modern (orbit) camera is on and would take the
#   mouse. Run A records (WOTM_INPUTREC), run B replays it (WOTM_INPUTPLAY). Moves the real cursor while the game window has focus.
param(
    [string]$Match = '3,1,1',
    [int]$Secs = 50
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$runtime = if ($env:WOTM_RECOMP_DIR) { $env:WOTM_RECOMP_DIR } elseif (Test-Path "$repo\.wotm_recomp_dir") { (Get-Content "$repo\.wotm_recomp_dir" -TotalCount 1).Trim() } else { '' }
if (-not $runtime -or -not (Test-Path "$runtime\run_live.ps1")) { throw "Runtime folder not found: run run_live.bat once or set WOTM_RECOMP_DIR." }

# relative mouse moves while the game window is in the foreground: 4 px steps every 30 ms, one second right, one second left, ...
$inject = {
    param([int]$Secs)
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MouseInject {
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public MOUSEINPUT mi; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    public static void Move(int dx, int dy) {
        INPUT[] i = { new INPUT { type = 0, mi = new MOUSEINPUT { dx = dx, dy = dy, flags = 1 } } };   // INPUT_MOUSE, MOUSEEVENTF_MOVE
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
    $sent = 0
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $Secs) {
        $fgPid = 0
        [void][MouseInject]::GetWindowThreadProcessId([MouseInject]::GetForegroundWindow(), [ref]$fgPid)
        $game = Get-Process -Id $fgPid -ErrorAction SilentlyContinue
        if ($game -and $game.ProcessName -eq 'wotm') {
            $dir = if ([int]((Get-Date) - $t0).TotalSeconds % 2 -eq 0) { 4 } else { -4 }
            [MouseInject]::Move($dir, 0)
            $sent++
        }
        Start-Sleep -Milliseconds 30
    }
    $sent
}

$inputs = Join-Path $runtime 'mouse_inputs.bin'
$script = '18:6:up;20:1:square;22:1:cross;24:3:left;25:1:triangle;27:4:up@2;28:1:square@2;30:2:right@2;31:1:circle@2;33:5:down;34:1:square'
$base = "WOTM_MATCH=$Match;WOTM_PLAYERS=2;WOTM_NOINPUT=1;WOTM_FIXEDSTEP=1;WOTM_SEED=1234;WOTM_AIVSAI=;WOTM_LOGICDELAY=;WOTM_INPUTDELAY=;WOTM_MOUSECAM="
$runs = @(
    @{ Name = 'A'; Pad = $script; Extra = "WOTM_INPUTREC=$inputs;WOTM_INPUTPLAY=" },
    @{ Name = 'B'; Pad = ''; Extra = "WOTM_INPUTREC=;WOTM_INPUTPLAY=$inputs" }
)
foreach ($r in $runs) {
    $hash = Join-Path $runtime "mouse_$($r.Name).hash"
    if (Test-Path $hash) { Remove-Item $hash -Force }
    $env:PS2X_PAD = if ($r.Pad) { $r.Pad } else { $null }
    $job = Start-Job -ScriptBlock $inject -ArgumentList $Secs
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'live_check.ps1') -Env "$base;$($r.Extra);WOTM_STATEHASH=$hash" `
        -Secs $Secs -Expect '\[sync\] seed 1234' -Reject '\[CRASH\]|||\[watchdog\] FROZE|||\[mouse\] cursor captured'
    $ok = $LASTEXITCODE -eq 0
    $sent = Receive-Job $job -Wait
    Remove-Job $job
    Copy-Item (Join-Path $runtime 'live_check.log') (Join-Path $runtime "mouse_$($r.Name).log") -Force
    Write-Host "run $($r.Name): $sent mouse moves sent to the game window"
    if (-not $ok) { Write-Host "FAILED: run $($r.Name) (see $runtime\mouse_$($r.Name).log)" -ForegroundColor Red; exit 1 }
    if ($sent -lt 100) { Write-Host "FAILED: run $($r.Name): the game window was not in the foreground long enough to move the mouse" -ForegroundColor Red; exit 1 }
}
$env:PS2X_PAD = $null
& python (Join-Path $PSScriptRoot 'statediff.py') (Join-Path $runtime 'mouse_A.hash') (Join-Path $runtime 'mouse_B.hash')
if ($LASTEXITCODE -ne 0) { Write-Host 'FAILED: the state diverges' -ForegroundColor Red; exit 1 }
Write-Host 'PASSED: identical state on every compared frame'
exit 0
