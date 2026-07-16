[CmdletBinding()]
param()

$repoRoot = Split-Path $PSScriptRoot -Parent
$captureRoot = Join-Path $repoRoot 'captures'
$pidFile = Join-Path $captureRoot 'live-714.pid'
$stdout = Join-Path $captureRoot 'live-714.log'

$processIds = @()
if (Test-Path $pidFile) {
    $storedPid = (Get-Content $pidFile -Raw).Trim()
    if ($storedPid -match '^\d+$') {
        $processIds += [int]$storedPid
    }
}
$processIds += @(Get-CimInstance Win32_Process -Filter "Name='dolby-probe.exe'" |
    Where-Object { $_.CommandLine -match '(?i)\blive-layout\b' } |
    Select-Object -ExpandProperty ProcessId)
$processIds = @($processIds | Sort-Object -Unique)

foreach ($processId in $processIds) {
    Stop-Process -Id $processId -ErrorAction SilentlyContinue
}
Remove-Item $pidFile -Force -ErrorAction SilentlyContinue

if ($processIds.Count -eq 0) {
    Write-Host 'live-layout was not running.'
} else {
    Write-Host "Stopped live-layout PID(s): $($processIds -join ', ')"
}
if (Test-Path $stdout) {
    Get-Content $stdout -Tail 20
}
