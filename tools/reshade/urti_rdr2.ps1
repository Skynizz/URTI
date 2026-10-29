# Essai automatique d'URTI dans RDR2 : lancement, menu « Histoire », captures régulières par le fichier
# déclencheur de l'add-on. Mode histoire uniquement.
param([int]$Captures = 12, [int]$Interval = 45, [int]$MenuWait = 60)

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
for ($i = 0; $i -lt $Captures; $i++) {
    Start-Sleep $Interval
    if (-not (Get-Process RDR2 -ErrorAction SilentlyContinue)) { "RDR2 arrêté"; exit 1 }
    New-Item -ItemType File -Path (Join-Path $game 'urti_capture_maintenant') -Force | Out-Null
    "capture demandée $i"
}
"fin de la série"
