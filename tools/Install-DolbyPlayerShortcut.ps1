[CmdletBinding()]
param()

$repoRoot = Split-Path $PSScriptRoot -Parent
$launcher = Join-Path $PSScriptRoot 'Launch-DolbyPlayer.ps1'
$applicationRoot = Join-Path $repoRoot 'build\SpatialAudioLab'
$player = Join-Path $applicationRoot 'SpatialAudioLab.Cinema.exe'
$mpv = Join-Path $env:ProgramFiles 'MPV Player\mpv.exe'
$desktop = [Environment]::GetFolderPath([Environment+SpecialFolder]::DesktopDirectory)
$shortcutPath = Join-Path $desktop 'SpatialAudioLab Cinema.lnk'
$legacyShortcutPath = Join-Path $desktop 'DolbyPlayer Atmos 7.1.4.lnk'
$powershell = Join-Path $PSHOME 'powershell.exe'

if (-not (Test-Path $launcher)) { throw "Launcher not found: $launcher" }
& (Join-Path $PSScriptRoot 'Publish-SpatialAudioLabCinema.ps1') -Destination $applicationRoot

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $powershell
$shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$launcher`""
$shortcut.WorkingDirectory = $repoRoot
$shortcut.Description = 'SpatialAudioLab Cinema - reproductor inmersivo PCM 7.1.4'
$shortcut.IconLocation = if (Test-Path $mpv) { "$mpv,0" } else { "$player,0" }
$shortcut.Save()

if (Test-Path $legacyShortcutPath) {
    Remove-Item $legacyShortcutPath -Force
}

Write-Host "Desktop shortcut ready: $shortcutPath"
