[CmdletBinding()]
param()

$repoRoot = Split-Path $PSScriptRoot -Parent
$captureRoot = Join-Path $repoRoot 'captures'
$pidFile = Join-Path $captureRoot 'live-native-mat-714.pid'
$stdout = Join-Path $captureRoot 'live-native-mat-714.log'

$processIds = @()
if (Test-Path -LiteralPath $pidFile) {
    $storedPid = (Get-Content $pidFile -Raw).Trim()
    if ($storedPid -match '^\d+$') { $processIds += [int]$storedPid }
}
$processIds = @($processIds | Sort-Object -Unique)

foreach ($processId in $processIds) {
    Stop-Process -Id $processId -ErrorAction SilentlyContinue
}
Remove-Item $pidFile -Force -ErrorAction SilentlyContinue

if ($processIds.Count -eq 0) {
    Write-Host 'Native MAT live-layout was not running.'
} else {
    Write-Host "Stopped native MAT live-layout PID(s): $($processIds -join ', ')"
}
if (Test-Path -LiteralPath $stdout) { Get-Content $stdout -Tail 20 }
