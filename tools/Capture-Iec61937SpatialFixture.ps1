[CmdletBinding()]
param(
    [ValidateRange(0.5, 60.0)]
    [double]$SpatialSeconds = 4.0,

    [ValidateSet('bed', 'height', '712', '714', 'dynamic', 'dynamic-origin',
        'dynamic-left', 'dynamic-right', 'dynamic-above', 'dynamic-front',
        'dynamic-behind', 'silence')]
    [string]$Mode = '714',

    [string]$Endpoint = '1 - HISENSE (Virtual Audio Device',

    [string]$OutputFile
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator. The IEC 61937 ring is restricted to administrators.'
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$probe = Join-Path $repoRoot 'build\SpatialAudioLab.CLI.exe'
if (-not (Test-Path $probe)) {
    throw "SpatialAudioLab.CLI.exe was not found. Run tools\Build-DolbyProbe.ps1 first."
}

$captureRoot = Join-Path $repoRoot 'captures'
New-Item -ItemType Directory -Path $captureRoot -Force | Out-Null
if (-not $OutputFile) {
    $OutputFile = Join-Path $captureRoot "dtsx-spatial-$Mode.wav"
} elseif (-not [IO.Path]::IsPathRooted($OutputFile)) {
    $OutputFile = Join-Path $repoRoot $OutputFile
}
$OutputFile = [IO.Path]::GetFullPath($OutputFile)
$stdoutPath = [IO.Path]::ChangeExtension($OutputFile, '.capture.log')
$stderrPath = [IO.Path]::ChangeExtension($OutputFile, '.capture.err.log')
Remove-Item $OutputFile, $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue

$captureSeconds = $SpatialSeconds + 2.0
$captureArguments = @(
    'capture-iec61937-ring',
    $captureSeconds.ToString([Globalization.CultureInfo]::InvariantCulture),
    $OutputFile,
    '2'
)
$capture = Start-Process -FilePath $probe -ArgumentList $captureArguments -PassThru `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath

try {
    Start-Sleep -Milliseconds 750
    & $probe spatial-test `
        $SpatialSeconds.ToString([Globalization.CultureInfo]::InvariantCulture) `
        $Endpoint $Mode
    if ($LASTEXITCODE -ne 0) {
        throw "spatial-test failed with exit code $LASTEXITCODE."
    }
} finally {
    if (-not $capture.HasExited) {
        $capture.WaitForExit([int](($captureSeconds + 5.0) * 1000)) | Out-Null
    }
}

if (-not $capture.HasExited) {
    $capture.Kill()
    throw 'IEC 61937 capture did not stop before the timeout.'
}
$capture.WaitForExit()
$capture.Refresh()
$captureOutput = if (Test-Path $stdoutPath) { Get-Content $stdoutPath -Raw } else { '' }
$captureError = if (Test-Path $stderrPath) { Get-Content $stderrPath -Raw } else { '' }
if ($captureOutput) { Write-Host $captureOutput.TrimEnd() }
if ($null -ne $capture.ExitCode -and $capture.ExitCode -ne 0) {
    throw "IEC 61937 capture failed with exit code $($capture.ExitCode): $captureError"
}
if ($captureError) { Write-Warning $captureError.TrimEnd() }
if (-not (Test-Path $OutputFile) -or (Get-Item $OutputFile).Length -eq 0) {
    throw 'IEC 61937 capture did not produce a WAV file.'
}

Write-Host "Fixture written: $OutputFile"
