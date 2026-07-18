[CmdletBinding()]
param(
    [string]$Destination = '',
    [string]$StudioSource = '',
    [string]$EngineSource = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $repoRoot 'build\SpatialAudioLab'
}
if ([string]::IsNullOrWhiteSpace($StudioSource)) {
    $StudioSource = Join-Path $PSScriptRoot 'SpeakerLayoutEditor\bin\Release\net8.0-windows'
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'Build-SpeakerLayoutEditor.ps1')
    if ([string]::IsNullOrWhiteSpace($EngineSource) -and
        -not (Test-Path -LiteralPath (Join-Path $repoRoot 'build-native\SpatialAudioLab.CLI.exe')) -and
        -not (Test-Path -LiteralPath (Join-Path $repoRoot 'build\SpatialAudioLab.CLI.exe'))) {
        & (Join-Path $PSScriptRoot 'Build-DolbyProbe.ps1')
    }
}
if ([string]::IsNullOrWhiteSpace($EngineSource)) {
    $EngineSource = & (Join-Path $PSScriptRoot 'Resolve-SpatialAudioLabEngine.ps1') -Candidates @(
        (Join-Path $repoRoot 'build-native\SpatialAudioLab.CLI.exe'),
        (Join-Path $repoRoot 'build\SpatialAudioLab.CLI.exe')
    )
}

$requiredSources = @(
    $StudioSource,
    (Join-Path $StudioSource 'SpatialAudioLab.Studio.exe'),
    $EngineSource
)
foreach ($path in $requiredSources) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Studio publishing dependency is missing: $path"
    }
}

New-Item -ItemType Directory -Path $Destination -Force | Out-Null
Copy-Item -Path (Join-Path $StudioSource '*') -Destination $Destination -Recurse -Force
$engineDestination = Join-Path $Destination 'Engine\SpatialAudioLab.CLI.exe'
New-Item -ItemType Directory -Path (Split-Path $engineDestination -Parent) -Force | Out-Null
Copy-Item -LiteralPath $EngineSource -Destination $engineDestination -Force

Write-Host "SpatialAudioLab Studio ready: $Destination"
