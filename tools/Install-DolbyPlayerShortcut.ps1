[CmdletBinding()]
param()

$repoRoot = Split-Path $PSScriptRoot -Parent
$launcher = Join-Path $PSScriptRoot 'Launch-DolbyPlayer.ps1'
$player = Join-Path $PSScriptRoot 'DolbyPlayer\bin\Release\net8.0-windows\dolby-player.exe'
$mpv = Join-Path $env:ProgramFiles 'MPV Player\mpv.exe'
$desktop = [Environment]::GetFolderPath([Environment+SpecialFolder]::DesktopDirectory)
$shortcutPath = Join-Path $desktop 'DolbyPlayer Atmos 7.1.4.lnk'
$powershell = Join-Path $PSHOME 'powershell.exe'

if (-not (Test-Path $launcher)) { throw "Launcher not found: $launcher" }
if (-not (Test-Path $player)) { & (Join-Path $PSScriptRoot 'Build-DolbyPlayer.ps1') }

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $powershell
$shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$launcher`""
$shortcut.WorkingDirectory = $repoRoot
$shortcut.Description = 'Reproductor Atmos analogico PCM 7.1.4'
$shortcut.IconLocation = if (Test-Path $mpv) { "$mpv,0" } else { "$player,0" }
$shortcut.Save()

Write-Host "Desktop shortcut ready: $shortcutPath"

