[CmdletBinding()]
param(
    [ValidateRange(1, 86400)]
    [int]$DurationSeconds = 3600,

    [ValidateRange(0.0, 1.0)]
    [double]$Gain = 0.25,

    [ValidateRange(20, 500)]
    [int]$PrebufferMilliseconds = 80
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator. The MAT ring is restricted to administrators.'
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repoRoot 'build\dolby-probe.exe'
$captureRoot = Join-Path $repoRoot 'captures'
$pidFile = Join-Path $captureRoot 'live-712.pid'
$stdout = Join-Path $captureRoot 'live-712.log'
$stderr = Join-Path $captureRoot 'live-712.err.log'

if (-not (Test-Path $exe)) {
    throw "dolby-probe.exe was not found: $exe"
}
New-Item -ItemType Directory -Path $captureRoot -Force | Out-Null

$existing = @(Get-CimInstance Win32_Process -Filter "Name='dolby-probe.exe'" |
    Where-Object { $_.CommandLine -match '(?i)\blive-712\b' })
if ($existing.Count -ne 0) {
    throw "live-712 is already running with PID(s): $($existing.ProcessId -join ', ')"
}

Remove-Item $stdout, $stderr, $pidFile -Force -ErrorAction SilentlyContinue
$gainText = $Gain.ToString([Globalization.CultureInfo]::InvariantCulture)
$arguments = "live-712 $DurationSeconds `"Altavoces (Realtek(R) Audio)`" `"2nd output`" $gainText $PrebufferMilliseconds"
$process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr

Start-Sleep -Milliseconds 750
$process.Refresh()
if ($process.HasExited) {
    $errorText = if (Test-Path $stderr) { Get-Content $stderr -Raw } else { '' }
    throw "live-712 exited during startup with code $($process.ExitCode). $errorText"
}

Set-Content -Path $pidFile -Value $process.Id -Encoding Ascii
Write-Host "live-712 started: PID=$($process.Id), duration=$DurationSeconds s, gain=$gainText."
Write-Host "Log: $stdout"
Write-Host "Stop with: .\tools\Stop-Live712.ps1"
