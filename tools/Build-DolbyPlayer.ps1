[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Initialize-SpatialAudioLab.ps1') -Component Cavern
$trueHdBinary = Join-Path $PSScriptRoot 'truehdd\truehd-stream.exe'
$playerProject = Join-Path $PSScriptRoot 'DolbyPlayer\DolbyPlayer.csproj'
$playerBinary = Join-Path $PSScriptRoot 'DolbyPlayer\bin\Release\net8.0-windows\SpatialAudioLab.Cinema.exe'

if (-not (Test-Path $trueHdBinary)) {
    throw "The bundled streaming TrueHD decoder is missing: $trueHdBinary"
}

Write-Host 'Building SpatialAudioLab Cinema...'
& dotnet build $playerProject -c Release
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $playerBinary)) {
    throw 'SpatialAudioLab Cinema build failed.'
}

$mpv = Join-Path $env:ProgramFiles 'MPV Player\mpv.exe'
if (-not (Test-Path $mpv)) {
    Write-Warning 'mpv is missing. Install it with: winget install --id shinchiro.mpv -e'
}

Write-Host "Ready: $playerBinary"
