# Real Alt+Tab (out of the game, wait, back to the game), to check that the add-on survives the window switch.
# Usage: alt_tab.ps1 -Process RDR2 -Seconds 6; prints yes if the game is still running at the end.
param([string]$Process = 'RDR2', [int]$Seconds = 6)

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
if (-not $p) { "no"; exit 1 }
$game = $p.MainWindowHandle
[UrtiAltTab]::SetForegroundWindow($game) | Out-Null
Start-Sleep -Milliseconds 500
[UrtiAltTab]::AltTab()
Start-Sleep -Seconds $Seconds
$left = [UrtiAltTab]::GetForegroundWindow() -ne $game
if (-not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) { "no (crash outside the game, left: $left)"; exit 1 }
# back to the game: Alt+Tab, then the window brought to the front if needed
[UrtiAltTab]::AltTab()
Start-Sleep -Seconds 1
if ([UrtiAltTab]::GetForegroundWindow() -ne $game) { [UrtiAltTab]::ShowWindow($game, 9) | Out-Null; [UrtiAltTab]::SetForegroundWindow($game) | Out-Null }
Start-Sleep -Seconds $Seconds
if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { "yes (left the game: $left, came back: $([UrtiAltTab]::GetForegroundWindow() -eq $game))" } else { "no (crash on return)"; exit 1 }
