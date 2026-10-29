# Clic gauche à une position relative de la fenêtre du jeu (0..1), après l'avoir mise au premier plan.
# Usage : click.ps1 -Process RDR2 -X 0.78 -Y 0.925
param([string]$Process = 'RDR2', [double]$X = 0.5, [double]$Y = 0.5)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KrcMouse {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MOUSEINPUT mi; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public static void Click() {
        var down = new INPUT { type = 0, mi = new MOUSEINPUT { dwFlags = 0x0002 } };
        var up = new INPUT { type = 0, mi = new MOUSEINPUT { dwFlags = 0x0004 } };
        SendInput(1, new[] { down }, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(80);
        SendInput(1, new[] { up }, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@

$p = Get-Process -Name $Process -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
if ([KrcMouse]::GetForegroundWindow() -ne $h) {
    [KrcMouse]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
    [KrcMouse]::SetForegroundWindow($h) | Out-Null
    [KrcMouse]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500
}
if ([KrcMouse]::GetForegroundWindow() -ne $h) { "$Process n'est pas au premier plan"; exit 1 }
$r = New-Object KrcMouse+RECT
[KrcMouse]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object KrcMouse+POINT
$pt.X = [int]($r.Right * $X); $pt.Y = [int]($r.Bottom * $Y)
[KrcMouse]::ClientToScreen($h, [ref]$pt) | Out-Null
[KrcMouse]::SetCursorPos($pt.X, $pt.Y) | Out-Null
Start-Sleep -Milliseconds 200
[KrcMouse]::Click()
"clic en ($($pt.X), $($pt.Y)) dans $Process ($($r.Right)x$($r.Bottom))"
