# Essai des ombres RT en direct dans RDR2 : lancement, menu « Histoire », chargement, puis photos de
# l'écran (écran partagé : gauche jeu seul, droite avec les ombres RT), ombres coupées puis remises
# (fichiers déclencheurs de l'add-on). Mode histoire uniquement.
param([int]$MenuWait = 60, [int]$LoadWait = 150, [int]$Settle = 60)

$game = 'F:\Red Dead Redemption 2'
$log = Join-Path $game 'urti_log.txt'
function Trigger($name) { New-Item -ItemType File -Path (Join-Path $game $name) -Force | Out-Null }

Start-Process -FilePath (Join-Path $game 'RDR2.exe') -WorkingDirectory $game
"jeu lancé"
$t = 0
while (-not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'appareil qui pr' -Quiet))) {
    Start-Sleep 2; $t += 2
    if ($t -gt 300) { "pas d'appareil après 5 min"; exit 1 }
}
"appareil prêt, attente du menu ($MenuWait s)"
Start-Sleep $MenuWait
& (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.778 -Y 0.925
"chargement ($LoadWait s)"
Start-Sleep $LoadWait
for ($k = 0; $k -lt 3; $k++) {
    & (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.5 -Y 0.5
    Start-Sleep 20
}
"en jeu : attente ($Settle s) puis photos"
Start-Sleep $Settle
Trigger 'urti_photo'; "photo 1 (ombres RT actives, écran partagé)"; Start-Sleep 25
Trigger 'urti_photo'; "photo 2 (pendant que la caméra tourne)"
& (Join-Path $PSScriptRoot 'mousemove.ps1') -Dx 15 -Steps 120 -Delay 15
Start-Sleep 25
Trigger 'urti_f7'; "ombres RT coupées"; Start-Sleep 25
Trigger 'urti_photo'; "photo 3 (ombres coupées)"; Start-Sleep 25
Trigger 'urti_f7'; "ombres RT remises"; Start-Sleep 30
"fin"
