# Test of live RT effects in RDR2: launch, "Story" menu, loading, then screen
# snapshots (split screen: left game alone, right with URTI), effects off then back on
# (the add-on's trigger files). Story mode only.
param([int]$MenuWait = 60, [int]$LoadWait = 150, [int]$Settle = 60, [switch]$NoPhotos)

$game = 'F:\Red Dead Redemption 2'
$log = Join-Path $game 'urti_log.txt'
function Trigger($name) { New-Item -ItemType File -Path (Join-Path $game $name) -Force | Out-Null }

Start-Process -FilePath (Join-Path $game 'RDR2.exe') -WorkingDirectory $game
"game launched"
$t = 0
while (-not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'presenting device' -Quiet))) {
    Start-Sleep 2; $t += 2
    if ($t -gt 300) { "no device after 5 min"; exit 1 }
}
"device ready, waiting for the menu ($MenuWait s)"
Start-Sleep $MenuWait
& (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.778 -Y 0.925
"loading ($LoadWait s)"
Start-Sleep $LoadWait
for ($k = 0; $k -lt 3; $k++) {
    & (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.5 -Y 0.5
    Start-Sleep 20
}
if ($NoPhotos) { "in game"; exit 0 }
"in game: waiting ($Settle s), then snapshots"
Start-Sleep $Settle
Trigger 'urti_photo'; "snapshot 1 (RT effects on, split screen)"; Start-Sleep 25
Trigger 'urti_photo'; "snapshot 2 (while the camera turns)"
& (Join-Path $PSScriptRoot 'mousemove.ps1') -Dx 15 -Steps 120 -Delay 15
Start-Sleep 25
Trigger 'urti_f7'; "RT effects off"; Start-Sleep 25
Trigger 'urti_photo'; "snapshot 3 (effects off)"; Start-Sleep 25
Trigger 'urti_f7'; "RT effects back on"; Start-Sleep 30
"done"
