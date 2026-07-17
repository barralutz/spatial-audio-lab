[CmdletBinding()]
param(
    [ValidateRange(0.5, 10.0)]
    [double]$DurationSeconds = 1.0,

    [string]$EndpointFilter = 'SinkDescription'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path $PSScriptRoot -Parent
$probe = Join-Path $repoRoot 'build\SpatialAudioLab.CLI.exe'
$driverDataRoot = if ($env:DriverData) {
    $env:DriverData
} else {
    Join-Path $env:SystemRoot 'System32\Drivers\DriverData'
}
$driverData = Join-Path $driverDataRoot 'Audio_Samples\Sysvad'
$captureDirectory = Join-Path $repoRoot 'captures'

if (-not (Test-Path $probe)) {
    throw "Probe executable not found: $probe"
}
if (-not (Test-Path $driverDataRoot)) {
    throw "Windows DriverData root not found: $driverDataRoot."
}
New-Item -ItemType Directory -Path $driverData -Force | Out-Null

$unsafeProcesses = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $_.ProcessName -in 'BF1', 'BF2042', 'EAAntiCheat.GameServiceLauncher'
})
if ($unsafeProcesses.Count -ne 0) {
    throw "Close Battlefield and EA AntiCheat before using the test-signed driver: $($unsafeProcesses.ProcessName -join ', ')"
}

New-Item -ItemType Directory -Path $captureDirectory -Force | Out-Null

$fixtures = [ordered]@{
    'silence'        = 'mat21-spatial-712-silence.wav'
    'dynamic-origin' = 'mat21-spatial-dynamic-fixed-origin.wav'
    'dynamic-left'   = 'mat21-spatial-dynamic-fixed-left.wav'
    'dynamic-right'  = 'mat21-spatial-dynamic-fixed-right.wav'
    'dynamic-above'  = 'mat21-spatial-dynamic-fixed-above.wav'
    'dynamic-below'  = 'mat21-spatial-dynamic-fixed-below.wav'
    'dynamic-front'  = 'mat21-spatial-dynamic-fixed-front.wav'
    'dynamic-behind' = 'mat21-spatial-dynamic-fixed-behind.wav'
    'dynamic-xquarter' = 'mat21-spatial-dynamic-fixed-xquarter.wav'
    'dynamic-yhalf' = 'mat21-spatial-dynamic-fixed-yhalf.wav'
    'dynamic-front-half' = 'mat21-spatial-dynamic-fixed-front-half.wav'
}

foreach ($fixture in $fixtures.GetEnumerator()) {
    $before = @{}
    Get-ChildItem $driverData -Filter 'STREAM_HOST_*.wav' -ErrorAction SilentlyContinue |
        ForEach-Object {
            $before[$_.FullName] = "$($_.Length):$($_.LastWriteTimeUtc.Ticks)"
        }

    Write-Host "Capturing $($fixture.Key)..."
    & $probe spatial-test $DurationSeconds $EndpointFilter $fixture.Key
    if ($LASTEXITCODE -ne 0) {
        throw "SpatialAudioLab CLI failed for $($fixture.Key): $LASTEXITCODE"
    }
    Start-Sleep -Milliseconds 300

    $dump = Get-ChildItem $driverData -Filter 'STREAM_HOST_*.wav' |
        Where-Object {
            $state = "$($_.Length):$($_.LastWriteTimeUtc.Ticks)"
            -not $before.ContainsKey($_.FullName) -or $before[$_.FullName] -ne $state
        } |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if ($null -eq $dump -or $dump.Length -le 68) {
        throw "SysVAD did not create a usable dump for $($fixture.Key)."
    }

    $destination = Join-Path $captureDirectory $fixture.Value
    Copy-Item $dump.FullName $destination -Force
    Write-Host "  $($dump.Name) -> $destination ($($dump.Length) bytes)"
}

Write-Host 'Spatial MAT fixtures captured successfully.'
