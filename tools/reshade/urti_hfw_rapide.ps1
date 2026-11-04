# Lancement rapide d'Horizon Forbidden West pour URTI (par Steam) : « Jouer » dans le lanceur du jeu (petite
# fenêtre), puis « Continuer » au menu principal, enfin attente de la partie (journal de l'add-on : plus de
# 1000 dessins par image). Pas de clic quand quelqu'un se sert du PC. Rend la main dès que la partie tourne.
param([int]$TimeoutMin = 10)

$game = 'F:\SteamLibrary\steamapps\common\Horizon Forbidden West Complete Edition'
$log = Join-Path $game 'urti_log.txt'
$proc = 'HorizonForbiddenWest'
# le journal d'une partie précédente peut rester : seules les lignes écrites depuis ce lancement comptent
$skip = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }
function Fresh { if (Test-Path $log) { Get-Content $log | Select-Object -Skip $skip } }
function Draws {
    $l = Fresh | Select-String -Pattern 'capture 0, frame \d+ : (\d+) dessins' | Select-Object -Last 1
    if ($l) { return [int]$l.Matches[0].Groups[1].Value }
    # étalonnage stocké : plus de capture ; la scène RT (sommets) en tient lieu
    $v = Fresh | Select-String -Pattern "structure d'accélération : (\d+) sommets en moyenne" | Select-Object -Last 1
    if ($v) { if ([int]$v.Matches[0].Groups[1].Value -lt 1000) { 50 } else { 1000 } } else { -1 }
}
function Idle { [int](& (Join-Path $PSScriptRoot 'inactif.ps1')) -ge 60 }
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

$t0 = Get-Date
if (-not (Get-Process $proc -ErrorAction SilentlyContinue)) { Start-Process 'steam://rungameid/2420110' }
"jeu lancé"
$played = $false; $continued = $false; $menuSeen = $null; $clicks = 0
while (((Get-Date) - $t0).TotalMinutes -lt $TimeoutMin) {
    Start-Sleep 3
    $s = [int]((Get-Date) - $t0).TotalSeconds
    $d = Draws
    if ($d -ge 1000) { "en partie (dessins $d) après $s s"; exit 0 }
    $w = WindowWidth
    # lanceur du jeu : petite fenêtre (784 de large), avant que l'appareil de rendu ne présente
    if (-not $played -and $w -gt 0 -and $w -lt 1200 -and (Idle)) {
        Start-Sleep 2
        & (Join-Path $PSScriptRoot 'click.ps1') -Process $proc -X 0.075 -Y 0.738 | Out-Null
        $played = $true; "lanceur : Jouer cliqué après $s s"
        continue
    }
    # menu principal : l'add-on présente (journal : bilan des présentations ; sans capture d'étalonnage quand
    # l'étalonnage est stocké) ; il finit d'apparaître. Le jeu ignore les touches envoyées : clic sur « Continuer »,
    # dont la place change (une ligne PlayStation Network s'ajoute au menu quand le compte est lié)
    $presenting = [bool](Fresh | Select-String -Pattern 'sur 300 présentations|première image' -Quiet)
    if (-not $continued -and $w -ge 1200 -and $presenting -and $d -lt 1000) {
        if (-not $menuSeen) { $menuSeen = Get-Date }
        elseif (((Get-Date) - $menuSeen).TotalSeconds -ge 12 -and ($clicks -gt 0 -or (Idle))) {
            # place de « Continuer » lue sur une capture de la fenêtre (deuxième ligne du menu)
            $shot = Join-Path $env:TEMP 'urti_hfw_menu.png'
            & (Join-Path $PSScriptRoot 'capture_fenetre.ps1') -Process $proc -Out $shot | Out-Null
            $y = & python (Join-Path $PSScriptRoot 'urti_hfw_menu.py') $shot
            Remove-Item $shot -ErrorAction SilentlyContinue
            # le premier clic ne fait parfois que choisir la ligne : un autre, tant que le menu est là (trois au plus)
            if ($y) {
                & (Join-Path $PSScriptRoot 'click.ps1') -Process $proc -X 0.095 -Y ([double]::Parse($y, [Globalization.CultureInfo]::InvariantCulture)) | Out-Null
                $clicks++; $menuSeen = (Get-Date).AddSeconds(-6)
                "menu : Continuer (à $y de la hauteur) cliqué après $s s"
                if ($clicks -ge 3) { $continued = $true }
            }
            else { $continued = $true }
        }
    }
}
"pas en partie après $TimeoutMin min"; exit 1
