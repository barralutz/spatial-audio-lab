[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateScript({ Test-Path $_ -PathType Leaf })]
    [string]$InputFile,

    [ValidateRange(0, 86400)]
    [double]$StartSeconds = 0,

    [ValidateRange(0, 1)]
    [double]$Gain = 0.25,

    [int]$AudioStreamIndex = -1,

    [ValidateRange(-5000, 5000)]
    [double]$AvDelayMilliseconds = 0,

    [ValidateRange(0, 86400)]
    [double]$StopAfterSeconds = 0,

    [string]$RearEndpoint = 'Altavoces (Realtek(R) Audio)',

    [string]$HeightEndpoint = '2nd output'
)

$ErrorActionPreference = 'Stop'
$player = Join-Path $PSScriptRoot 'DolbyPlayer\bin\Release\net8.0-windows\dolby-player.exe'
$trueHdBinary = Join-Path $PSScriptRoot 'truehdd\truehd-stream.exe'
if (-not (Test-Path $player) -or -not (Test-Path $trueHdBinary)) {
    & (Join-Path $PSScriptRoot 'Build-DolbyPlayer.ps1')
}

$invariant = [Globalization.CultureInfo]::InvariantCulture
$arguments = @(
    'play', (Resolve-Path $InputFile).Path,
    '--start', $StartSeconds.ToString($invariant),
    '--gain', $Gain.ToString($invariant),
    '--av-delay-ms', $AvDelayMilliseconds.ToString($invariant),
    '--rear', $RearEndpoint,
    '--height', $HeightEndpoint
)
if ($AudioStreamIndex -ge 0) {
    $arguments += @('--audio-track', $AudioStreamIndex.ToString($invariant))
}
if ($StopAfterSeconds -gt 0) {
    $arguments += @('--stop-after', $StopAfterSeconds.ToString($invariant))
}

& $player @arguments
exit $LASTEXITCODE
