[CmdletBinding()]
param(
    [ValidateRange(1.0, 10.0)]
    [double]$SpatialSeconds = 3.0,

    [string]$Endpoint = 'SinkDescription Sample'
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator. The IEC 61937 ring is restricted to administrators.'
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$captureScript = Join-Path $PSScriptRoot 'Capture-Iec61937SpatialFixture.ps1'
$captureRoot = Join-Path $repoRoot 'captures'
$batchLog = Join-Path $captureRoot 'dtsx-position-fixtures.log'
New-Item -ItemType Directory -Path $captureRoot -Force | Out-Null
Set-Content $batchLog "DTS:X position batch started: $(Get-Date -Format o)"
$positions = 'origin', 'left', 'right', 'above', 'front', 'behind'
foreach ($position in $positions) {
    $mode = "dynamic-$position"
    $output = Join-Path $repoRoot "captures\dtsx-position-$position.wav"
    Write-Host "`nCapturing DTS:X position: $position"
    Add-Content $batchLog "Capturing ${position}: $(Get-Date -Format o)"
    try {
        & $captureScript -SpatialSeconds $SpatialSeconds -Mode $mode `
            -Endpoint $Endpoint -OutputFile $output
        Add-Content $batchLog "Completed ${position}: $(Get-Date -Format o)"
    } catch {
        Add-Content $batchLog "Failed ${position}: $($_ | Out-String)"
        throw
    }
    Start-Sleep -Seconds 1
}

Write-Host "`nDTS:X position fixtures captured successfully."
