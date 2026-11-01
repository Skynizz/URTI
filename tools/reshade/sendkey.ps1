# Envoie une touche au jeu (codes de touche matériels, comme un vrai clavier) après l'avoir mis au
# premier plan. Usage : sendkey.ps1 -Process RDR2 -Key Enter [-Hold 80]
param([string]$Process = 'RDR2', [string]$Key = 'Enter', [int]$Hold = 80)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KrcInput {
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void Key(ushort scan, bool extended, int hold) {
        uint flags = 0x0008 | (extended ? 0x0001u : 0u); // KEYEVENTF_SCANCODE
        var down = new INPUT { type = 1, ki = new KEYBDINPUT { wScan = scan, dwFlags = flags } };
        var up = new INPUT { type = 1, ki = new KEYBDINPUT { wScan = scan, dwFlags = flags | 0x0002 } };
        SendInput(1, new[] { down }, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(hold);
        SendInput(1, new[] { up }, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@

$codes = @{ Enter = @(0x1C, $false); Escape = @(0x01, $false); Space = @(0x39, $false); F9 = @(0x43, $false);
            Up = @(0x48, $true); Down = @(0x50, $true); Left = @(0x4B, $true); Right = @(0x4D, $true); W = @(0x11, $false); Home = @(0x47, $true); F4 = @(0x3E, $false); F7 = @(0x41, $false); F8 = @(0x42, $false) }
$p = Get-Process -Name $Process -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
# Windows refuse le premier plan à un processus d'arrière-plan : un appui sur Alt le débloque
if ([KrcInput]::GetForegroundWindow() -ne $p.MainWindowHandle) {
    [KrcInput]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
    [KrcInput]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    [KrcInput]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500
}
if ([KrcInput]::GetForegroundWindow() -ne $p.MainWindowHandle) { "$Process n'est pas au premier plan : touche non envoyée"; exit 1 }
$c = $codes[$Key]
[KrcInput]::Key([uint16]$c[0], [bool]$c[1], $Hold)
"touche $Key envoyée à $Process"
