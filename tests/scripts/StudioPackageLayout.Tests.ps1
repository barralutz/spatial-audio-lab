[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$publishScript = Join-Path $repoRoot 'tools\Publish-SpatialAudioLabStudio.ps1'
$resolveEngineScript = Join-Path $repoRoot 'tools\Resolve-SpatialAudioLabEngine.ps1'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) "SpatialAudioLab-Studio-$([guid]::NewGuid().ToString('N'))"

try {
    $studioSource = Join-Path $temporaryRoot 'studio-source'
    $toolSource = Join-Path $temporaryRoot 'tool-source'
    $engineSource = Join-Path $temporaryRoot 'SpatialAudioLab.CLI.exe'
    $destination = Join-Path $temporaryRoot 'application'
    New-Item -ItemType Directory -Path $studioSource, $toolSource -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $studioSource 'SpatialAudioLab.Studio.exe') -Value 'studio'
    Set-Content -LiteralPath (Join-Path $studioSource 'SpatialAudioLab.Studio.dll') -Value 'dependency'
    Set-Content -LiteralPath $engineSource -Value 'engine'

    $staleEngine = Join-Path $toolSource 'stale-engine.exe'
    $freshEngine = Join-Path $toolSource 'fresh-engine.exe'
    Set-Content -LiteralPath $staleEngine -Value 'stale'
    Set-Content -LiteralPath $freshEngine -Value 'fresh'
    (Get-Item -LiteralPath $staleEngine).LastWriteTimeUtc = [datetime]::UtcNow.AddMinutes(-5)
    (Get-Item -LiteralPath $freshEngine).LastWriteTimeUtc = [datetime]::UtcNow
    $resolvedEngine = & $resolveEngineScript -Candidates @($staleEngine, $freshEngine)
    if ($resolvedEngine -ne $freshEngine) {
        throw "Newest Engine was not selected: $resolvedEngine"
    }

    & $publishScript `
        -Destination $destination `
        -StudioSource $studioSource `
        -EngineSource $engineSource `
        -SkipBuild

    $expected = @(
        'SpatialAudioLab.Studio.exe',
        'SpatialAudioLab.Studio.dll',
        'Engine\SpatialAudioLab.CLI.exe'
    )
    foreach ($relativePath in $expected) {
        if (-not (Test-Path -LiteralPath (Join-Path $destination $relativePath) -PathType Leaf)) {
            throw "Studio package is missing: $relativePath"
        }
    }

    if (Test-Path -LiteralPath (Join-Path $destination 'portable.flag')) {
        throw 'The development package must preserve installed-mode LocalAppData profiles.'
    }

    Write-Host 'Studio package layout test passed.'
} finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
}
