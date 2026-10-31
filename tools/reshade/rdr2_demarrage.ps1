# Démarrage de RDR2 sans clic : le jeu passe-t-il la création de son appareil ? Lance le jeu, surveille
# 90 s le processus et les plantages (journal d'erreurs Windows), puis rend la main (le jeu reste ouvert
# s'il tourne). Sortie : « vivant », ou le module fautif du plantage.
param([int]$Seconds = 90)
$game = 'F:\Red Dead Redemption 2'
$t0 = Get-Date
Start-Process -FilePath (Join-Path $game 'RDR2.exe') -WorkingDirectory $game
$seen = $false
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep 3
    $crash = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $t0 } -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match 'RDR2.exe' } | Select-Object -First 1
    if ($crash) {
        $m = [regex]::Match($crash.Message, 'module défaillant : ([^,]+)').Groups[1].Value
        $c = [regex]::Match($crash.Message, 'Exception code: (0x[0-9a-fA-F]+)').Groups[1].Value
        "plantage après $([int]((Get-Date) - $t0).TotalSeconds) s : $m $c"
        exit 1
    }
    if (Get-Process RDR2 -ErrorAction SilentlyContinue) { $seen = $true }
    if ($seen -and ((Get-Date) - $t0).TotalSeconds -gt 60 -and (Get-Process RDR2 -ErrorAction SilentlyContinue)) { "vivant après $([int]((Get-Date) - $t0).TotalSeconds) s"; exit 0 }
}
if (Get-Process RDR2 -ErrorAction SilentlyContinue) { "vivant" } else { "pas de processus RDR2" }
