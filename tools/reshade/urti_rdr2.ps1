# Essai automatique d'URTI dans RDR2 : lancement, menu « Histoire », passage de l'écran de chargement,
# captures régulières (fichier déclencheur de l'add-on) pendant un balayage de souris qui fait tourner
# la caméra (auto-étalonnage). Mode histoire uniquement.
param([int]$Captures = 8, [int]$Interval = 45, [int]$MenuWait = 60, [int]$LoadWait = 150, [switch]$Sweep)

$game = 'F:\Red Dead Redemption 2'
$log = Join-Path $game 'urti_log.txt'
Start-Process -FilePath (Join-Path $game 'RDR2.exe') -WorkingDirectory $game
"jeu lancé"
# le lanceur relance le jeu : attendre l'appareil qui présente
$t = 0
while (-not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'appareil qui pr' -Quiet))) {
    Start-Sleep 2; $t += 2
    if ($t -gt 300) { "pas d'appareil après 5 min"; exit 1 }
}
"appareil prêt, attente du menu ($MenuWait s)"
Start-Sleep $MenuWait
& (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.778 -Y 0.925
"chargement ($LoadWait s), puis clics pour passer l'écran « appuyez pour continuer »"
Start-Sleep $LoadWait
for ($k = 0; $k -lt 3; $k++) {
    & (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.5 -Y 0.5
    & (Join-Path $PSScriptRoot 'sendkey.ps1') -Process RDR2 -Key Space
    Start-Sleep 20
}
for ($i = 0; $i -lt $Captures; $i++) {
    if (-not (Get-Process RDR2 -ErrorAction SilentlyContinue)) { "RDR2 arrêté"; exit 1 }
    New-Item -ItemType File -Path (Join-Path $game 'urti_capture_maintenant') -Force | Out-Null
    if ($Sweep) { & (Join-Path $PSScriptRoot 'mousemove.ps1') -Dx 12 -Steps 90 -Delay 15 }
    "capture demandée $i"
    Start-Sleep $Interval
}
"fin de la série"
