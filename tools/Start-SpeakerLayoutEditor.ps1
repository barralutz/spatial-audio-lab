[CmdletBinding()]
param()

$executable = Join-Path $PSScriptRoot `
    'SpeakerLayoutEditor\bin\Release\net8.0-windows\speaker-layout-editor.exe'
if (-not (Test-Path $executable)) {
    & (Join-Path $PSScriptRoot 'Build-SpeakerLayoutEditor.ps1')
}
Start-Process -FilePath $executable
