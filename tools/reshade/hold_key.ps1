# Maintient une touche (code matériel) enfoncée dans un jeu, le temps demandé, seulement si ce jeu est au premier plan : sinon
# rien n'est envoyé (la touche partirait dans la fenêtre de l'utilisateur). hold_key.ps1 -Process HorizonForbiddenWest -Scan 0x11 -Seconds 6
param([string]$Process, [int]$Scan = 0x11, [double]$Seconds = 5, [int]$Scan2 = 0)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class UrtiHold {
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int p);
    public static int ForegroundPid() { int p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
    public static void Key(int scan, bool up) {
        var i = new INPUT(); i.type = 1; i.ki.scan = (ushort)scan; i.ki.flags = 0x0008u | (up ? 0x0002u : 0u);
        SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
$game = Get-Process -Name $Process -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game -or [UrtiHold]::ForegroundPid() -ne $game.Id) { "jeu pas au premier plan : aucune touche envoyée"; exit 1 }
[UrtiHold]::Key($Scan, $false); if ($Scan2) { [UrtiHold]::Key($Scan2, $false) }
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 50
    # l'utilisateur a changé de fenêtre : on relâche tout de suite
    if ([UrtiHold]::ForegroundPid() -ne $game.Id) { "premier plan perdu : touche relâchée"; break }
    [UrtiHold]::Key($Scan, $false); if ($Scan2) { [UrtiHold]::Key($Scan2, $false) }
}
[UrtiHold]::Key($Scan, $true); if ($Scan2) { [UrtiHold]::Key($Scan2, $true) }
"touche tenue"
