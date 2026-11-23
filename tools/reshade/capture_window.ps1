# Image of the game window only (not the rest of the desktop): CopyFromScreen on its client rectangle
param([string]$Process = 'RDR2', [string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class UrtiWin { [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; } }
"@
$p = Get-Process -Name $Process | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$r = New-Object UrtiWin+RECT; [UrtiWin]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
$o = New-Object UrtiWin+POINT; [UrtiWin]::ClientToScreen($p.MainWindowHandle, [ref]$o) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($o.X, $o.Y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); "image $($bmp.Width)x$($bmp.Height)"
