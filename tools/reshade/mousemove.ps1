# Relative mouse sweep (turns a game's camera): Steps moves of Dx, Dy pixels.
# -Process: only if that game is in the foreground, stopping as soon as it no longer is (otherwise the user's mouse moves on their desktop).
param([int]$Dx = 15, [int]$Dy = 0, [int]$Steps = 60, [int]$Delay = 20, [string]$Process = '')

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KrcMove {
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MOUSEINPUT mi; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int p);
    public static int ForegroundPid() { int p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
    public static void Move(int dx, int dy) {
        var m = new INPUT { type = 0, mi = new MOUSEINPUT { dx = dx, dy = dy, dwFlags = 0x0001 } };
        SendInput(1, new[] { m }, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
$game = if ($Process) { Get-Process -Name $Process -ErrorAction SilentlyContinue | Select-Object -First 1 } else { $null }
if ($Process -and -not $game) { "game not running: mouse untouched"; exit 1 }
for ($i = 0; $i -lt $Steps; $i++) {
    if ($game -and [KrcMove]::ForegroundPid() -ne $game.Id) { "game not in the foreground: mouse untouched"; exit 1 }
    [KrcMove]::Move($Dx, $Dy); Start-Sleep -Milliseconds $Delay
}
