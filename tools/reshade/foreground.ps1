# Only tells whether the foreground window belongs to the game (yes / no): nothing else is read or recorded
param([string]$Process = 'RDR2')
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class UrtiFg { [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid); }
"@
$fgPid = 0
[UrtiFg]::GetWindowThreadProcessId([UrtiFg]::GetForegroundWindow(), [ref]$fgPid) | Out-Null
$game = Get-Process -Name $Process -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id
if ($game -and ($game -contains $fgPid)) { 'yes' } else { 'no' }
