[CmdletBinding()]
param(
    [ValidateRange(10, 120)]
    [int]$TraceSeconds = 25,

    [string]$ProcmonPath = "$env:LOCALAPPDATA\Temp\SpatialAudioLab\Procmon\Procmon64.exe",

    [ValidateSet('Virtual', 'Hisense')]
    [string]$Target = 'Virtual',

    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$TraceName = '',

    [switch]$EtwOnly
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator.'
}
if (-not $EtwOnly -and -not (Test-Path -LiteralPath $ProcmonPath)) {
    throw "Process Monitor was not found: $ProcmonPath"
}

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class SpatialAudioLabWindowCloser
{
    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int maximumCount);

    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    public static bool CloseWindow(int processId, string title)
    {
        bool found = false;
        EnumWindows(delegate (IntPtr window, IntPtr parameter) {
            uint ownerProcessId;
            GetWindowThreadProcessId(window, out ownerProcessId);
            if (ownerProcessId != (uint)processId) {
                return true;
            }

            StringBuilder text = new StringBuilder(256);
            GetWindowText(window, text, text.Capacity);
            if (!String.Equals(text.ToString(), title, StringComparison.Ordinal)) {
                return true;
            }

            found = PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
            return false;
        }, IntPtr.Zero);
        return found;
    }
}
'@

$repoRoot = Split-Path $PSScriptRoot -Parent
$captureRoot = Join-Path $repoRoot 'captures'
$traceStem = if ([string]::IsNullOrWhiteSpace($TraceName)) {
    "bf1-audio-detection-$($Target.ToLowerInvariant())"
} else {
    $TraceName
}
$pml = Join-Path $captureRoot "$traceStem.pml"
$csv = Join-Path $captureRoot "$traceStem.csv"
$audioEtl = Join-Path $captureRoot "$traceStem-audio.etl"
$audioCsv = Join-Path $captureRoot "$traceStem-audio.csv"
$driverFormatLog = Join-Path $captureRoot "$traceStem-driver-formats.txt"
$audioEtwSession = 'SpatialAudioLabBattlefieldAudio'
$audioProvider = '{AE4BD3BE-F36F-45B6-8D21-BDD6FB832853}'
$shortcut = 'C:\Users\Public\Desktop\Battlefield 1.lnk'
$cli = Join-Path $repoRoot 'build\SpatialAudioLab.CLI.exe'
New-Item -ItemType Directory -Path $captureRoot -Force | Out-Null

function Stop-Battlefield {
    $process = Get-Process bf1 -ErrorAction SilentlyContinue
    if ($null -eq $process) { return }
    $null = $process.CloseMainWindow()
    if (-not $process.WaitForExit(15000)) {
        Stop-Process -Id $process.Id -Force
    }
}

Stop-Battlefield
& (Join-Path $PSScriptRoot 'Stop-LiveNativeMat714.ps1')
& "$env:SystemRoot\System32\logman.exe" stop $audioEtwSession -ets 2>$null | Out-Null
Remove-Item $pml, $csv, $audioEtl, $audioCsv, $driverFormatLog `
    -Force -ErrorAction SilentlyContinue

try {
    if ($Target -eq 'Virtual') {
        & $cli driver-format-log reset
        & (Join-Path $PSScriptRoot 'Start-LiveNativeMat714.ps1') `
            -Gain 0.25 -PrebufferMilliseconds 20 -LatencyMode Low
    } else {
        & $cli set-default '1 - HISENSE (AMD High Definition Audio Device)'
        & $cli set-codec-format mat10 '1 - HISENSE (AMD High Definition Audio Device)'
    }

    & "$env:SystemRoot\System32\logman.exe" start $audioEtwSession `
        -p $audioProvider 0x1001000000000000 5 -o $audioEtl -ets | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Could not start the Microsoft-Windows-Audio ETW session: $LASTEXITCODE"
    }

    if (-not $EtwOnly) {
        $arguments = @(
            '/AcceptEula'
            '/Quiet'
            '/Minimized'
            '/NoFilter'
            '/BackingFile'
            "`"$pml`""
            '/Runtime'
            '150'
        )
        Start-Process -FilePath $ProcmonPath -ArgumentList $arguments | Out-Null
        Start-Sleep -Seconds 2
    }

    Start-Process -FilePath $shortcut | Out-Null
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 500
        $battlefield = Get-Process bf1 -ErrorAction SilentlyContinue
    } while ($null -eq $battlefield -and (Get-Date) -lt $deadline)
    if ($null -eq $battlefield) {
        throw 'Battlefield 1 did not start during the trace.'
    }

    $dialogDeadline = (Get-Date).AddSeconds(15)
    $dialogClosed = $false
    do {
        if ([SpatialAudioLabWindowCloser]::CloseWindow(
                $battlefield.Id, 'Minimum Hardware Check')) {
            $closeDeadline = (Get-Date).AddSeconds(5)
            do {
                Start-Sleep -Milliseconds 100
                $stillOpen = [SpatialAudioLabWindowCloser]::CloseWindow(
                    $battlefield.Id, 'Minimum Hardware Check')
            } while ($stillOpen -and (Get-Date) -lt $closeDeadline)
            if ($stillOpen) {
                throw 'Minimum Hardware Check did not close after WM_CLOSE.'
            }
            $dialogClosed = $true
            Write-Host 'Closed Minimum Hardware Check with WM_CLOSE; Battlefield remains running.'
        }
        Start-Sleep -Milliseconds 200
    } while (-not $dialogClosed -and (Get-Date) -lt $dialogDeadline)

    $audioDeadline = (Get-Date).AddMinutes(2)
    do {
        Start-Sleep -Seconds 1
        $battlefield.Refresh()
        try {
            $audioReady = $battlefield.Modules.ModuleName -contains 'MMDevAPI.dll'
        } catch {
            $audioReady = $false
        }
    } while (-not $audioReady -and -not $battlefield.HasExited -and (Get-Date) -lt $audioDeadline)
    if (-not $audioReady) {
        throw 'Battlefield 1 did not initialize MMDevAPI within two minutes.'
    }

    Write-Host "Battlefield audio initialized; capturing $TraceSeconds additional seconds."
    Start-Sleep -Seconds $TraceSeconds
    if (-not $EtwOnly) {
        & $ProcmonPath /AcceptEula /Quiet /Terminate | Out-Null
    }
    & "$env:SystemRoot\System32\logman.exe" stop $audioEtwSession -ets | Out-Null
} finally {
    if (-not $EtwOnly) {
        & $ProcmonPath /AcceptEula /Quiet /Terminate 2>$null | Out-Null
    }
    & "$env:SystemRoot\System32\logman.exe" stop $audioEtwSession -ets 2>$null | Out-Null
    Stop-Battlefield
    if ($Target -eq 'Virtual') {
        & (Join-Path $PSScriptRoot 'Stop-LiveNativeMat714.ps1')
        try {
            & $cli driver-format-log 2>&1 |
                Set-Content -LiteralPath $driverFormatLog -Encoding utf8
        } catch {
            Write-Warning "Could not save the driver format log: $_"
        }
    }
}

if (-not $EtwOnly) {
    $export = Start-Process -FilePath $ProcmonPath -ArgumentList @(
        '/AcceptEula'
        '/Quiet'
        '/OpenLog'
        "`"$pml`""
        '/SaveAs'
        "`"$csv`""
    ) -PassThru -Wait
    if ($export.ExitCode -ne 0) {
        throw "Process Monitor could not export the trace: $($export.ExitCode)"
    }

    $exportDeadline = (Get-Date).AddMinutes(2)
    do {
        Start-Sleep -Milliseconds 500
    } while (-not (Test-Path -LiteralPath $csv) -and (Get-Date) -lt $exportDeadline)
    if (-not (Test-Path -LiteralPath $csv)) {
        throw 'Process Monitor did not create the CSV export within two minutes.'
    }

    Write-Host "PML: $pml"
    Write-Host "CSV: $csv"
}

& "$env:SystemRoot\System32\tracerpt.exe" $audioEtl -of CSV -o $audioCsv -y | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $audioCsv)) {
    throw "Could not export the Microsoft-Windows-Audio ETW trace: $LASTEXITCODE"
}
Write-Host "Audio ETL: $audioEtl"
Write-Host "Audio CSV: $audioCsv"
if (Test-Path -LiteralPath $driverFormatLog) {
    Write-Host "Driver formats: $driverFormatLog"
}
