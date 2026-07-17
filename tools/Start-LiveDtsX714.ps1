[CmdletBinding()]
param(
    [ValidateRange(0, 3600)]
    [int]$DurationSeconds = 0,

    [ValidateRange(0.0, 1.0)]
    [double]$Gain = 0.25,

    [ValidateRange(20, 500)]
    [int]$PrebufferMilliseconds = 40,

    [ValidateSet('Safe', 'Balanced', 'Low')]
    [string]$LatencyMode = 'Balanced',

    [string]$Layout = ''
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator. The IEC 61937 ring is restricted to administrators.'
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repoRoot 'build\SpatialAudioLab.CLI.exe'
$captureRoot = Join-Path $repoRoot 'captures'
$pidFile = Join-Path $captureRoot 'live-dtsx-714.pid'
$stdout = Join-Path $captureRoot 'live-dtsx-714.log'
$stderr = Join-Path $captureRoot 'live-dtsx-714.err.log'
if ([string]::IsNullOrWhiteSpace($Layout)) {
    $Layout = Join-Path $repoRoot 'configs\realtek-c1u-714.ini'
}
$layoutPath = [IO.Path]::GetFullPath($Layout)

if (-not (Test-Path $exe)) {
    throw "SpatialAudioLab.CLI.exe was not found: $exe"
}
if (-not (Test-Path $layoutPath)) {
    throw "Speaker layout was not found: $layoutPath"
}
New-Item -ItemType Directory -Path $captureRoot -Force | Out-Null

$existing = @(Get-CimInstance Win32_Process `
    -Filter "Name='SpatialAudioLab.CLI.exe' OR Name='dolby-probe.exe'" |
    Where-Object { $_.CommandLine -match '(?i)\blive-(dtsx-|pcm-)?layout\b' })
if ($existing.Count -ne 0) {
    & (Join-Path $PSScriptRoot 'Stop-Live714.ps1')
    & (Join-Path $PSScriptRoot 'Stop-LiveDtsX714.ps1')
    & (Join-Path $PSScriptRoot 'Stop-LivePcm714.ps1')
    Start-Sleep -Milliseconds 300
}

& (Join-Path $PSScriptRoot 'Set-SpatialProvider.ps1') -Mode DtsX

Remove-Item $stdout, $stderr, $pidFile -Force -ErrorAction SilentlyContinue
$gainText = $Gain.ToString([Globalization.CultureInfo]::InvariantCulture)
$latencyText = $LatencyMode.ToLowerInvariant()
$arguments = "live-dtsx-layout $DurationSeconds `"$layoutPath`" $gainText $PrebufferMilliseconds $latencyText"
$process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr

Start-Sleep -Milliseconds 750
$process.Refresh()
if ($process.HasExited) {
    $errorText = if (Test-Path $stderr) { Get-Content $stderr -Raw } else { '' }
    throw "live-dtsx-layout exited during startup with code $($process.ExitCode). $errorText"
}

Set-Content -Path $pidFile -Value $process.Id -Encoding Ascii
$durationText = if ($DurationSeconds -eq 0) { 'until stopped' } else { "$DurationSeconds s" }
Write-Host "live-dtsx-layout started: PID=$($process.Id), duration=$durationText, gain=$gainText, latency=$latencyText."
Write-Host "Layout: $layoutPath"
Write-Host "Log: $stdout"
Write-Host 'Stop with: .\tools\Stop-LiveDtsX714.ps1'
