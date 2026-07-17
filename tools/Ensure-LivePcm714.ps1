[CmdletBinding()]
param(
    [ValidateRange(0.0, 1.0)]
    [double]$Gain = 0.15,

    [ValidateRange(20, 500)]
    [int]$PrebufferMilliseconds = 40,

    [ValidateSet('Safe', 'Balanced', 'Low')]
    [string]$LatencyMode = 'Balanced'
)

$running = @(Get-CimInstance Win32_Process -Filter "Name='dolby-probe.exe'" |
    Where-Object { $_.CommandLine -match '(?i)\blive-pcm-layout\b' })
if ($running.Count -ne 0) {
    Write-Host "PCM 7.1.4 bridge already active: PID=$($running[0].ProcessId)."
    return
}

& (Join-Path $PSScriptRoot 'Start-LivePcm714.ps1') `
    -Gain $Gain -PrebufferMilliseconds $PrebufferMilliseconds -LatencyMode $LatencyMode

