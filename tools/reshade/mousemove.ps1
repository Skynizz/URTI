# Balayage de souris relatif (fait tourner la caméra d'un jeu) : Steps mouvements de Dx, Dy pixels.
param([int]$Dx = 15, [int]$Dy = 0, [int]$Steps = 60, [int]$Delay = 20)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KrcMove {
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MOUSEINPUT mi; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    public static void Move(int dx, int dy) {
        var m = new INPUT { type = 0, mi = new MOUSEINPUT { dx = dx, dy = dy, dwFlags = 0x0001 } };
        SendInput(1, new[] { m }, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
for ($i = 0; $i -lt $Steps; $i++) { [KrcMove]::Move($Dx, $Dy); Start-Sleep -Milliseconds $Delay }
