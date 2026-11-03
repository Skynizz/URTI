# Lancement rapide de RDR2 pour URTI : au lieu d'attentes fixes, l'état du jeu est lu dans le journal de
# l'add-on (nombre de dessins des captures d'étalonnage : menu ~150, écran de chargement ~30, partie > 1000).
# Clic sur « Histoire » dès que le menu est là, clics au centre tant que la partie n'est pas affichée.
# Rend la main dès que la partie tourne (ou que le soleil est trouvé). Mode histoire uniquement.
param([int]$TimeoutMin = 12)

$game = 'F:\Red Dead Redemption 2'
$log = Join-Path $game 'urti_log.txt'
# le journal d'une partie précédente peut rester : seules les lignes écrites depuis ce lancement comptent
$skip = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }
function Fresh { if (Test-Path $log) { Get-Content $log | Select-Object -Skip $skip } }
function Draws {
    $l = Fresh | Select-String -Pattern 'capture 0, frame \d+ : (\d+) dessins' | Select-Object -Last 1
    if ($l) { return [int]$l.Matches[0].Groups[1].Value }
    # étalonnage stocké : plus de capture au menu ; la scène RT presque vide (quelques sommets) en tient lieu
    $s = Fresh | Select-String -Pattern "structure d'accélération : (\d+) sommets en moyenne" | Select-Object -Last 1
    if ($s) { if ([int]$s.Matches[0].Groups[1].Value -lt 1000) { 150 } else { 1000 } } else { -1 }
}
# « première image » n'en est pas un signe : elle s'écrit dès l'écran de démarrage
function InGame { [bool](Fresh | Select-String -Pattern 'direct : soleil \(' -Quiet) }
# pas de clic quand quelqu'un se sert du PC (souris ou clavier touchés dans la dernière minute)
function Idle { [int](& (Join-Path $PSScriptRoot 'inactif.ps1')) -ge 60 }

$t0 = Get-Date
if (-not (Get-Process RDR2 -ErrorAction SilentlyContinue)) { Start-Process -FilePath (Join-Path $game 'RDR2.exe') -WorkingDirectory $game }
"jeu lancé"
$menuClicked = $false; $lastClick = Get-Date
while (((Get-Date) - $t0).TotalMinutes -lt $TimeoutMin) {
    Start-Sleep 3
    # le premier RDR2.exe passe par le lanceur Rockstar : le processus n'est suivi qu'une fois le jeu démarré
    $started = [bool](Fresh | Select-String -Pattern 'appareil qui pr' -Quiet)
    if ($started -and -not (Get-Process RDR2 -ErrorAction SilentlyContinue)) { "le jeu s'est fermé"; exit 1 }
    if (InGame) { "en partie après $([int]((Get-Date) - $t0).TotalSeconds) s"; exit 0 }
    $d = Draws
    if ($d -ge 1000) { "en partie (dessins $d) après $([int]((Get-Date) - $t0).TotalSeconds) s"; exit 0 }
    if (-not $menuClicked -and $d -ge 100 -and (Idle)) {
        Start-Sleep 4 # le menu finit d'apparaître
        & (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.778 -Y 0.925 | Out-Null
        $menuClicked = $true; $lastClick = Get-Date
        "menu : Histoire cliqué après $([int]((Get-Date) - $t0).TotalSeconds) s"
    }
    elseif ($menuClicked -and ((Get-Date) - $lastClick).TotalSeconds -ge 20 -and $d -lt 100 -and (Idle)) {
        & (Join-Path $PSScriptRoot 'click.ps1') -Process RDR2 -X 0.5 -Y 0.5 | Out-Null # « appuyez sur une touche »
        $lastClick = Get-Date
    }
}
"pas en partie après $TimeoutMin min"; exit 1
