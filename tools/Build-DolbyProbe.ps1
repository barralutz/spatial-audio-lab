[CmdletBinding()]
param()

$repoRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $repoRoot 'build-native'
$publishRoot = Join-Path $repoRoot 'build'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'vswhere.exe was not found. Install Visual Studio 2022 with the C++ workload.'
}
$visualStudio = & $vswhere -latest -products '*' -property installationPath
if (-not $visualStudio) {
    throw 'Visual Studio 2022 was not found.'
}
$devCmd = Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Path $buildRoot, $publishRoot -Force | Out-Null

$sources = @(
    'audio_platform.cpp',
    'capture_commands.cpp',
    'dolby_probe.cpp',
    'mat_analysis.cpp',
    'mat_capture_client.cpp',
    'mat_format.cpp',
    'mat_pipeline.cpp',
    'multi_endpoint_renderer.cpp',
    'speaker_layout.cpp',
    'wave_io.cpp'
) | ForEach-Object { '"..\src\' + $_ + '"' }

$compile = @(
    'cl /nologo /std:c++20 /EHsc /W4 /permissive-',
    '/DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN',
    ($sources -join ' '),
    '/Fe:dolby-probe.exe',
    '/link ole32.lib uuid.lib avrt.lib propsys.lib mmdevapi.lib'
) -join ' '
$command = '"' + $devCmd + '" -arch=x64 -host_arch=x64 >nul && cd /d "' +
    $buildRoot + '" && ' + $compile

& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "dolby-probe build failed: $LASTEXITCODE"
}
Copy-Item (Join-Path $buildRoot 'dolby-probe.exe') `
    (Join-Path $publishRoot 'dolby-probe.exe') -Force
Write-Host "Built: $(Join-Path $publishRoot 'dolby-probe.exe')"
