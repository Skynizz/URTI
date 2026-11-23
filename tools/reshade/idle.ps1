# Seconds since the user's last keyboard / mouse action (so as not to disturb a game in progress)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class UrtiIdle { [StructLayout(LayoutKind.Sequential)] struct LII { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LII p);
public static uint Seconds() { var l = new LII(); l.cbSize = 8; GetLastInputInfo(ref l); return ((uint)Environment.TickCount - l.dwTime) / 1000; } }
"@
[UrtiIdle]::Seconds()
