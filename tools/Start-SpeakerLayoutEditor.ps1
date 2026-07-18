[CmdletBinding()]
param()

$running = @(Get-Process -Name 'SpatialAudioLab.Studio' -ErrorAction SilentlyContinue)
if ($running.Count -ne 0) {
    Write-Host "SpatialAudioLab Studio is already running with PID(s): $($running.Id -join ', ')"
    return
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$projectRoot = Join-Path $PSScriptRoot 'SpeakerLayoutEditor'
$applicationRoot = Join-Path $repoRoot 'build\SpatialAudioLab'
$executable = Join-Path $applicationRoot 'SpatialAudioLab.Studio.exe'
$engine = Join-Path $applicationRoot 'Engine\SpatialAudioLab.CLI.exe'
$engineSource = & (Join-Path $PSScriptRoot 'Resolve-SpatialAudioLabEngine.ps1') -Candidates @(
    (Join-Path $repoRoot 'build-native\SpatialAudioLab.CLI.exe'),
    (Join-Path $repoRoot 'build\SpatialAudioLab.CLI.exe')
)
$requiresPublish = -not (Test-Path -LiteralPath $executable) -or
    -not (Test-Path -LiteralPath $engine)
if (-not $requiresPublish) {
    $executableTime = (Get-Item -LiteralPath $executable).LastWriteTimeUtc
    $requiresPublish = @(Get-ChildItem $projectRoot -Recurse -File |
        Where-Object {
            $_.Extension -in '.cs', '.xaml', '.csproj' -and
            $_.LastWriteTimeUtc -gt $executableTime
        }).Count -ne 0
}
if (-not $requiresPublish) {
    $requiresPublish = (Get-Item -LiteralPath $engineSource).LastWriteTimeUtc -gt
        (Get-Item -LiteralPath $engine).LastWriteTimeUtc
}
if ($requiresPublish) {
    & (Join-Path $PSScriptRoot 'Publish-SpatialAudioLabStudio.ps1') `
        -Destination $applicationRoot `
        -EngineSource $engineSource
}
Start-Process -FilePath $executable
