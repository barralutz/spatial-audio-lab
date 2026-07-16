[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$trueHdBinary = Join-Path $PSScriptRoot 'truehdd\truehd-stream.exe'
$playerProject = Join-Path $PSScriptRoot 'DolbyPlayer\DolbyPlayer.csproj'
$playerBinary = Join-Path $PSScriptRoot 'DolbyPlayer\bin\Release\net8.0-windows\dolby-player.exe'

if (-not (Test-Path $trueHdBinary)) {
    throw "The bundled streaming TrueHD decoder is missing: $trueHdBinary"
}

Write-Host 'Building DolbyPlayer...'
& dotnet build $playerProject -c Release
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $playerBinary)) {
    throw 'DolbyPlayer build failed.'
}

$mpv = Join-Path $env:ProgramFiles 'MPV Player\mpv.exe'
if (-not (Test-Path $mpv)) {
    Write-Warning 'mpv is missing. Install it with: winget install --id shinchiro.mpv -e'
}

Write-Host "Ready: $playerBinary"
