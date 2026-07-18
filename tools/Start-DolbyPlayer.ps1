[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateScript({ [IO.File]::Exists([IO.Path]::GetFullPath($_)) })]
    [string]$InputFile,

    [ValidateRange(0, 86400)]
    [double]$StartSeconds = 0,

    [ValidateRange(0, 1)]
    [double]$Gain = 1.0,

    [int]$AudioStreamIndex = -1,

    [ValidateRange(-5000, 5000)]
    [double]$AvDelayMilliseconds = 0,

    [ValidateRange(0, 86400)]
    [double]$StopAfterSeconds = 0,

    [string]$SinkEndpoint = '1 - HISENSE (Virtual Audio Device',

    [switch]$SkipBridgeSetup
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$applicationRoot = Join-Path $repoRoot 'build\SpatialAudioLab'
$player = Join-Path $applicationRoot 'SpatialAudioLab.Cinema.exe'
$requiredApplicationFiles = @(
    $player,
    (Join-Path $applicationRoot 'Tools\ffmpeg\bin\ffprobe.exe'),
    (Join-Path $applicationRoot 'Tools\mpv\mpv.exe'),
    (Join-Path $applicationRoot 'Tools\truehdd\truehd-stream.exe')
)
if (@($requiredApplicationFiles | Where-Object { -not (Test-Path -LiteralPath $_) }).Count -ne 0) {
    & (Join-Path $PSScriptRoot 'Publish-SpatialAudioLabCinema.ps1') -Destination $applicationRoot
}
if (-not $SkipBridgeSetup) {
    & (Join-Path $PSScriptRoot 'Ensure-LivePcm714.ps1')
}

$invariant = [Globalization.CultureInfo]::InvariantCulture
$arguments = @(
    'play', (Resolve-Path -LiteralPath $InputFile).Path,
    '--start', $StartSeconds.ToString($invariant),
    '--gain', $Gain.ToString($invariant),
    '--av-delay-ms', $AvDelayMilliseconds.ToString($invariant),
    '--sink', $SinkEndpoint
)
if ($AudioStreamIndex -ge 0) {
    $arguments += @('--audio-track', $AudioStreamIndex.ToString($invariant))
}
if ($StopAfterSeconds -gt 0) {
    $arguments += @('--stop-after', $StopAfterSeconds.ToString($invariant))
}

$log = Join-Path (Split-Path $PSScriptRoot -Parent) 'captures\spatial-audio-lab-cinema.log'
New-Item -ItemType Directory -Path (Split-Path $log -Parent) -Force | Out-Null
& $player @arguments 2>&1 | Tee-Object -FilePath $log
$playerExitCode = $LASTEXITCODE
if ($playerExitCode -ne 0) {
    $details = @(Get-Content $log -Tail 12 -ErrorAction SilentlyContinue) -join "`n"
    throw "SpatialAudioLab Cinema exited with code $playerExitCode.`n$details"
}
