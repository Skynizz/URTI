# Dit seulement si la fenêtre au premier plan appartient au jeu (oui / non) : rien d'autre n'est lu ni noté
param([string]$Process = 'RDR2')
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class UrtiFg { [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid); }
"@
$fgPid = 0
[UrtiFg]::GetWindowThreadProcessId([UrtiFg]::GetForegroundWindow(), [ref]$fgPid) | Out-Null
$game = Get-Process -Name $Process -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id
if ($game -and ($game -contains $fgPid)) { 'oui' } else { 'non' }
