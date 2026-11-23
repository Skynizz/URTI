# Automated URTI test in RDR2: launch, "Story" menu, past the loading screen,
# regular captures (the add-on's trigger file) during a mouse sweep that turns
# the camera (self-calibration). Story mode only.
param([int]$Captures = 8, [int]$Interval = 45, [int]$MenuWait = 60, [int]$LoadWait = 150, [switch]$Sweep)

$game = 'F:\Red Dead Redemption 2'
$log = Join-Path $game 'urti_log.txt'
Start-Process -FilePath (Join-Path $game 'RDR2.exe') -WorkingDirectory $game
"game launched"
# the launcher restarts the game: wait for the presenting device
$t = 0
while (-not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'presenting device' -Quiet))) {
    Start-Sleep 2; $t += 2
    if ($t -gt 300) { "no device after 5 min"; exit 1 }
}
"device ready, waiting for the menu ($MenuWait s)"
Start-Sleep $MenuWait
& (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.778 -Y 0.925
"loading ($LoadWait s), then clicks to get past the 'press to continue' screen"
Start-Sleep $LoadWait
for ($k = 0; $k -lt 3; $k++) {
    & (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.5 -Y 0.5
    & (Join-Path $PSScriptRoot 'sendkey.ps1') -Process RDR2 -Key Space
    Start-Sleep 20
}
for ($i = 0; $i -lt $Captures; $i++) {
    if (-not (Get-Process RDR2 -ErrorAction SilentlyContinue)) { "RDR2 stopped"; exit 1 }
    New-Item -ItemType File -Path (Join-Path $game 'urti_capture_now') -Force | Out-Null
    if ($Sweep) { & (Join-Path $PSScriptRoot 'mousemove.ps1') -Dx 12 -Steps 90 -Delay 15 }
    "capture requested $i"
    Start-Sleep $Interval
}
"series done"
