# Vrai Alt+Tab (hors du jeu, attente, retour au jeu), pour vérifier que l'add-on survit au changement de fenêtre.
# Usage : alt_tab.ps1 -Process HorizonForbiddenWest -Secondes 6 ; écrit oui si le jeu tourne encore à la fin.
param([string]$Process = 'HorizonForbiddenWest', [int]$Secondes = 6)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class UrtiAltTab {
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    public static void AltTab() {
        keybd_event(0x12, 0, 0, IntPtr.Zero);
        keybd_event(0x09, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        keybd_event(0x09, 0, 2, IntPtr.Zero);
        keybd_event(0x12, 0, 2, IntPtr.Zero);
    }
}
"@
$p = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { "non"; exit 1 }
$game = $p.MainWindowHandle
[UrtiAltTab]::SetForegroundWindow($game) | Out-Null
Start-Sleep -Milliseconds 500
[UrtiAltTab]::AltTab()
Start-Sleep -Seconds $Secondes
$left = [UrtiAltTab]::GetForegroundWindow() -ne $game
if (-not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) { "non (plantage hors du jeu, parti : $left)"; exit 1 }
# retour au jeu : Alt+Tab, puis la fenêtre remise devant s'il le faut
[UrtiAltTab]::AltTab()
Start-Sleep -Seconds 1
if ([UrtiAltTab]::GetForegroundWindow() -ne $game) { [UrtiAltTab]::ShowWindow($game, 9) | Out-Null; [UrtiAltTab]::SetForegroundWindow($game) | Out-Null }
Start-Sleep -Seconds $Secondes
if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { "oui (parti du jeu : $left, revenu : $([UrtiAltTab]::GetForegroundWindow() -eq $game))" } else { "non (plantage au retour)"; exit 1 }
