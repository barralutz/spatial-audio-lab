[CmdletBinding()]
param()

$repoRoot = Split-Path $PSScriptRoot -Parent
$project = Join-Path $PSScriptRoot 'SpeakerLayoutEditor\SpeakerLayoutEditor.csproj'
dotnet build $project -c Release
if ($LASTEXITCODE -ne 0) {
    throw "Speaker Layout Editor build failed: $LASTEXITCODE"
}
Write-Host "Built: $(Join-Path $PSScriptRoot 'SpeakerLayoutEditor\bin\Release\net8.0-windows\speaker-layout-editor.exe')"
