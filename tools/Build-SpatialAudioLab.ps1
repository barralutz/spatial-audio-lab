[CmdletBinding()]
param(
    [ValidateSet('CLI', 'Studio', 'Cinema', 'All')]
    [string[]]$Component = @('CLI', 'Studio')
)

$ErrorActionPreference = 'Stop'
$requested = @($Component | ForEach-Object { $_.ToLowerInvariant() })
if ($requested -contains 'all') {
    $requested = @('cli', 'studio', 'cinema')
}

foreach ($name in @($requested | Select-Object -Unique)) {
    switch ($name) {
        'cli' { & (Join-Path $PSScriptRoot 'Build-DolbyProbe.ps1') }
        'studio' { & (Join-Path $PSScriptRoot 'Build-SpeakerLayoutEditor.ps1') }
        'cinema' { & (Join-Path $PSScriptRoot 'Build-DolbyPlayer.ps1') }
    }
}

Write-Host "SpatialAudioLab build complete: $($requested -join ', ')."
