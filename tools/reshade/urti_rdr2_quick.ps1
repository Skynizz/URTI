# Quick RDR2 launch for URTI: instead of fixed waits, the game's state is read from the add-on's
# log (draw count of the calibration captures: menu ~150, loading screen ~30, in game > 1000).
# Clicks "Story" as soon as the menu is there, clicks in the centre as long as the game is not shown.
# Returns as soon as the game runs (or the sun is found). Story mode only.
param([int]$TimeoutMin = 12, [switch]$Force)

$game = 'F:\Red Dead Redemption 2'
$log = Join-Path $game 'urti_log.txt'
# the log of a previous session may remain: only lines written since this launch count
$skip = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }
function Fresh { if (Test-Path $log) { Get-Content $log | Select-Object -Skip $skip } }
function Draws {
    $l = Fresh | Select-String -Pattern 'capture 0, frame \d+: (\d+) draws' | Select-Object -Last 1
    if ($l) { return [int]$l.Matches[0].Groups[1].Value }
    # stored calibration: no capture in the menu any more; the almost empty RT scene (a few vertices) stands in for it
    $s = Fresh | Select-String -Pattern 'acceleration structure: (\d+) vertices on average' | Select-Object -Last 1
    if ($s) { if ([int]$s.Matches[0].Groups[1].Value -lt 1000) { 150 } else { 1000 } } else { -1 }
}
# "first frame" is not a sign of it: it is written from the start screen
function InGame { [bool](Fresh | Select-String -Pattern 'live: sun (tracked )?\(' -Quiet) }
# no click while someone uses the PC (mouse or keyboard touched within the last minute)
function Idle { $Force -or [int](& (Join-Path $PSScriptRoot 'idle.ps1')) -ge 60 }

$t0 = Get-Date
if (-not (Get-Process RDR2 -ErrorAction SilentlyContinue)) { Start-Process -FilePath (Join-Path $game 'RDR2.exe') -WorkingDirectory $game }
"game launched"
$menuClicked = $false; $lastClick = Get-Date
while (((Get-Date) - $t0).TotalMinutes -lt $TimeoutMin) {
    Start-Sleep 3
    # the first RDR2.exe goes through the Rockstar launcher: the process is only tracked once the game has started
    $started = [bool](Fresh | Select-String -Pattern 'presenting device' -Quiet)
    if ($started -and -not (Get-Process RDR2 -ErrorAction SilentlyContinue)) { "the game closed"; exit 1 }
    if (InGame) { "in game after $([int]((Get-Date) - $t0).TotalSeconds) s"; exit 0 }
    $d = Draws
    if ($d -ge 1000) { "in game (draws $d) after $([int]((Get-Date) - $t0).TotalSeconds) s"; exit 0 }
    if (-not $menuClicked -and $d -ge 100 -and (Idle)) {
        Start-Sleep 4 # the menu finishes appearing
        & (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.778 -Y 0.925 | Out-Null
        $menuClicked = $true; $lastClick = Get-Date
        "menu: Story clicked after $([int]((Get-Date) - $t0).TotalSeconds) s"
    }
    elseif ($menuClicked -and ((Get-Date) - $lastClick).TotalSeconds -ge 20 -and $d -lt 100 -and (Idle)) {
        & (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.5 -Y 0.5 | Out-Null # "press any key"
        $lastClick = Get-Date
    }
}
"not in game after $TimeoutMin min"; exit 1
