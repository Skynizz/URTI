# Click-free start of RDR2: does the game get past creating its device? Launches the game, watches
# the process and crashes (Windows error log) for 90 s, then returns (the game stays open
# if it runs). Output: "alive", or the faulting module of the crash.
param([int]$Seconds = 90)
$game = 'F:\Red Dead Redemption 2'
$t0 = Get-Date
Start-Process -FilePath (Join-Path $game 'RDR2.exe') -WorkingDirectory $game
$seen = $false
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep 3
    $crash = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $t0 } -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match 'RDR2.exe' } | Select-Object -First 1
    if ($crash) {
        $m = [regex]::Match($crash.Message, '(?:Faulting module name|module d.faillant)\s*:\s*([^,]+)').Groups[1].Value
        $c = [regex]::Match($crash.Message, 'Exception code: (0x[0-9a-fA-F]+)').Groups[1].Value
        "crash after $([int]((Get-Date) - $t0).TotalSeconds) s: $m $c"
        exit 1
    }
    if (Get-Process RDR2 -ErrorAction SilentlyContinue) { $seen = $true }
    if ($seen -and ((Get-Date) - $t0).TotalSeconds -gt 60 -and (Get-Process RDR2 -ErrorAction SilentlyContinue)) { "alive after $([int]((Get-Date) - $t0).TotalSeconds) s"; exit 0 }
}
if (Get-Process RDR2 -ErrorAction SilentlyContinue) { "alive" } else { "no RDR2 process" }
