[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateScript({ Test-Path $_ -PathType Leaf })]
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

    [string]$SinkEndpoint = 'SinkDescription Sample',

    [switch]$SkipBridgeSetup
)

$ErrorActionPreference = 'Stop'
$player = Join-Path $PSScriptRoot 'DolbyPlayer\bin\Release\net8.0-windows\dolby-player.exe'
$trueHdBinary = Join-Path $PSScriptRoot 'truehdd\truehd-stream.exe'
if (-not (Test-Path $player) -or -not (Test-Path $trueHdBinary)) {
    & (Join-Path $PSScriptRoot 'Build-DolbyPlayer.ps1')
}
if (-not $SkipBridgeSetup) {
    & (Join-Path $PSScriptRoot 'Ensure-LivePcm714.ps1')
}

$invariant = [Globalization.CultureInfo]::InvariantCulture
$arguments = @(
    'play', (Resolve-Path $InputFile).Path,
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

$log = Join-Path (Split-Path $PSScriptRoot -Parent) 'captures\dolby-player.log'
New-Item -ItemType Directory -Path (Split-Path $log -Parent) -Force | Out-Null
& $player @arguments 2>&1 | Tee-Object -FilePath $log
$playerExitCode = $LASTEXITCODE
if ($playerExitCode -ne 0) {
    $details = @(Get-Content $log -Tail 12 -ErrorAction SilentlyContinue) -join "`n"
    throw "DolbyPlayer exited with code $playerExitCode.`n$details"
}
