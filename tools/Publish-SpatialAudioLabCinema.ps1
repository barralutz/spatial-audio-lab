[CmdletBinding()]
param(
    [string]$Destination = '',
    [string]$CinemaSource = '',
    [string]$FfprobePath = '',
    [string]$MpvPath = '',
    [string]$TrueHdPath = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $repoRoot 'build\SpatialAudioLab'
}
if ([string]::IsNullOrWhiteSpace($CinemaSource)) {
    $CinemaSource = Join-Path $PSScriptRoot 'DolbyPlayer\bin\Release\net8.0-windows'
}
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'Build-DolbyPlayer.ps1')
}

if ([string]::IsNullOrWhiteSpace($FfprobePath)) {
    $ffprobeCommand = Get-Command ffprobe.exe -ErrorAction SilentlyContinue
    if ($null -eq $ffprobeCommand) {
        throw 'ffprobe.exe is required. Install FFmpeg before publishing Cinema.'
    }
    $FfprobePath = $ffprobeCommand.Source
}
if ([string]::IsNullOrWhiteSpace($MpvPath)) {
    $MpvPath = Join-Path $env:ProgramFiles 'MPV Player\mpv.exe'
}
if ([string]::IsNullOrWhiteSpace($TrueHdPath)) {
    $TrueHdPath = Join-Path $PSScriptRoot 'truehdd\truehd-stream.exe'
}

$requiredSources = @(
    $CinemaSource,
    (Join-Path $CinemaSource 'SpatialAudioLab.Cinema.exe'),
    $FfprobePath,
    $MpvPath,
    $TrueHdPath
)
foreach ($path in $requiredSources) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Cinema publishing dependency is missing: $path"
    }
}

New-Item -ItemType Directory -Path $Destination -Force | Out-Null
Copy-Item -Path (Join-Path $CinemaSource '*') -Destination $Destination -Recurse -Force

$ffprobeDestination = Join-Path $Destination 'Tools\ffmpeg\bin\ffprobe.exe'
$mpvDestination = Join-Path $Destination 'Tools\mpv\mpv.exe'
$trueHdDestination = Join-Path $Destination 'Tools\truehdd\truehd-stream.exe'
foreach ($path in @($ffprobeDestination, $mpvDestination, $trueHdDestination)) {
    New-Item -ItemType Directory -Path (Split-Path $path -Parent) -Force | Out-Null
}
Copy-Item -LiteralPath $FfprobePath -Destination $ffprobeDestination -Force
Copy-Item -LiteralPath $MpvPath -Destination $mpvDestination -Force
Copy-Item -LiteralPath $TrueHdPath -Destination $trueHdDestination -Force

Write-Host "SpatialAudioLab Cinema ready: $Destination"
