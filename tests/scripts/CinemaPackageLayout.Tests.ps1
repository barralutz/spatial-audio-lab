[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$publishScript = Join-Path $repoRoot 'tools\Publish-SpatialAudioLabCinema.ps1'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) "SpatialAudioLab-Cinema-$([guid]::NewGuid().ToString('N'))"

try {
    $cinemaSource = Join-Path $temporaryRoot 'cinema-source'
    $toolSource = Join-Path $temporaryRoot 'tool-source'
    $destination = Join-Path $temporaryRoot 'application'
    New-Item -ItemType Directory -Path $cinemaSource, $toolSource -Force | Out-Null

    $cinema = Join-Path $cinemaSource 'SpatialAudioLab.Cinema.exe'
    $cinemaDependency = Join-Path $cinemaSource 'SpatialAudioLab.Cinema.dll'
    $ffprobe = Join-Path $toolSource 'ffprobe.exe'
    $mpv = Join-Path $toolSource 'mpv.exe'
    $trueHd = Join-Path $toolSource 'truehd-stream.exe'
    Set-Content -LiteralPath $cinema -Value 'cinema'
    Set-Content -LiteralPath $cinemaDependency -Value 'dependency'
    Set-Content -LiteralPath $ffprobe -Value 'ffprobe'
    Set-Content -LiteralPath $mpv -Value 'mpv'
    Set-Content -LiteralPath $trueHd -Value 'truehd'

    & $publishScript `
        -Destination $destination `
        -CinemaSource $cinemaSource `
        -FfprobePath $ffprobe `
        -MpvPath $mpv `
        -TrueHdPath $trueHd `
        -SkipBuild

    $expected = @(
        'SpatialAudioLab.Cinema.exe',
        'SpatialAudioLab.Cinema.dll',
        'Tools\ffmpeg\bin\ffprobe.exe',
        'Tools\mpv\mpv.exe',
        'Tools\truehdd\truehd-stream.exe'
    )
    foreach ($relativePath in $expected) {
        $path = Join-Path $destination $relativePath
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Cinema package is missing: $relativePath"
        }
    }

    if (Test-Path -LiteralPath (Join-Path $destination 'portable.flag')) {
        throw 'The development package must preserve installed-mode LocalAppData profiles.'
    }

    Write-Host 'Cinema package layout test passed.'
} finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
}
