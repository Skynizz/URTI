# Lancement rapide d'Horizon Forbidden West pour URTI (par Steam) : « Jouer » dans le lanceur du jeu (petite
# fenêtre), puis « Continuer » au menu principal (lu sur une capture), enfin attente de la partie (scène RT garnie
# dans le journal de l'add-on, et plus de menu à l'écran). Sans -Force : pas de clic quand quelqu'un se sert du PC.
# Rend la main dès que la partie tourne.
param([int]$TimeoutMin = 10, [switch]$Force)

$game = 'F:\SteamLibrary\steamapps\common\Horizon Forbidden West Complete Edition'
$log = Join-Path $game 'urti_log.txt'
$proc = 'HorizonForbiddenWest'
# le journal d'une partie précédente peut rester : seules les lignes écrites depuis ce lancement comptent
$skip = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }
function Fresh { if (Test-Path $log) { Get-Content $log | Select-Object -Skip $skip } }
# sommets de la scène RT (motif sans accent : le journal est lu en ANSI, « é » y fait deux caractères)
function Vertices {
    $v = Fresh | Select-String -Pattern "(\d+) sommets en moyenne" | Select-Object -Last 1
    if ($v) { [int]$v.Matches[0].Groups[1].Value } else { -1 }
}
function Idle { $Force -or [int](& (Join-Path $PSScriptRoot 'inactif.ps1')) -ge 60 }
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class UrtiHfwWin { [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r); }
"@
function WindowWidth {
    $p = Get-Process -Name $proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { return 0 }
    $r = New-Object UrtiHfwWin+RECT; [UrtiHfwWin]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $r.R - $r.L
}
# place de « Continuer » si le menu principal est à l'écran, sinon rien
function MenuY {
    $shot = Join-Path $env:TEMP 'urti_hfw_menu.png'
    & (Join-Path $PSScriptRoot 'capture_fenetre.ps1') -Process $proc -Out $shot | Out-Null
    $y = & python (Join-Path $PSScriptRoot 'urti_hfw_menu.py') $shot
    Remove-Item $shot -ErrorAction SilentlyContinue
    $y
}

# Steam Cloud : si la synchronisation échoue (connexion perdue), Steam attend une réponse avant de lancer le jeu, et sa
# question « Jouer quand même » met en jeu les sauvegardes de l'utilisateur : jamais de clic automatique, l'essai s'arrête
$steamLog = 'C:\Program Files (x86)\Steam\logs\console_log.txt'
$steamSkip = if (Test-Path $steamLog) { @(Get-Content $steamLog).Count } else { 0 }
function CloudBlocked {
    if (-not (Test-Path $steamLog)) { return $false }
    [bool](Get-Content $steamLog | Select-Object -Skip $steamSkip | Select-String -SimpleMatch 'waiting for user response to SynchronizingCloud')
}

$t0 = Get-Date
if (-not (Get-Process $proc -ErrorAction SilentlyContinue)) { Start-Process 'steam://rungameid/2420110' }
"jeu lancé"
$lastPlay = [datetime]::MinValue; $lastMenu = [datetime]::MinValue
while (((Get-Date) - $t0).TotalMinutes -lt $TimeoutMin) {
    Start-Sleep 3
    $s = [int]((Get-Date) - $t0).TotalSeconds
    if (CloudBlocked) { "Steam Cloud : synchronisation impossible, Steam attend une réponse : essai arrêté (aucun clic)"; exit 3 }
    $w = WindowWidth
    # lanceur du jeu : petite fenêtre (784 de large)
    if ($w -gt 0 -and $w -lt 1200) {
        if (((Get-Date) - $lastPlay).TotalSeconds -ge 15 -and (Idle)) {
            Start-Sleep 2
            & (Join-Path $PSScriptRoot 'click.ps1') -Process $proc -X 0.075 -Y 0.738 | Out-Null
            $lastPlay = Get-Date; "lanceur : Jouer cliqué après $s s"
        }
        continue
    }
    if ($w -lt 1200) { continue }
    # menu principal : le jeu ignore les touches envoyées, clic sur « Continuer ». Le premier clic ne fait parfois que
    # choisir la ligne : un autre 15 s plus tard si le menu est toujours là
    $y = MenuY
    if ($y) {
        if (((Get-Date) - $lastMenu).TotalSeconds -ge 15 -and (Idle)) {
            & (Join-Path $PSScriptRoot 'click.ps1') -Process $proc -X 0.095 -Y ([double]::Parse($y, [Globalization.CultureInfo]::InvariantCulture)) | Out-Null
            $lastMenu = Get-Date; "menu : Continuer (à $y de la hauteur) cliqué après $s s"
        }
        continue
    }
    $v = Vertices
    if ($v -ge 100000) { "en partie ($v sommets dans la scène RT) après $s s"; exit 0 }
}
"pas en partie après $TimeoutMin min"; exit 1
