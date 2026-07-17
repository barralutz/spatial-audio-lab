[CmdletBinding()]
param()

$running = @(Get-Process -Name 'SpatialAudioLab.Studio' -ErrorAction SilentlyContinue)
if ($running.Count -ne 0) {
    Write-Host "SpatialAudioLab Studio is already running with PID(s): $($running.Id -join ', ')"
    return
}

$projectRoot = Join-Path $PSScriptRoot 'SpeakerLayoutEditor'
$executable = Join-Path $PSScriptRoot `
    'SpeakerLayoutEditor\bin\Release\net8.0-windows\SpatialAudioLab.Studio.exe'
$requiresBuild = -not (Test-Path $executable)
if (-not $requiresBuild) {
    $executableTime = (Get-Item $executable).LastWriteTimeUtc
    $requiresBuild = @(Get-ChildItem $projectRoot -Recurse -File |
        Where-Object {
            $_.Extension -in '.cs', '.xaml', '.csproj' -and
            $_.LastWriteTimeUtc -gt $executableTime
        }).Count -ne 0
}
if ($requiresBuild) {
    & (Join-Path $PSScriptRoot 'Build-SpeakerLayoutEditor.ps1')
}
Start-Process -FilePath $executable
