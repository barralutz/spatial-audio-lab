[CmdletBinding()]
param(
    [ValidateRange(0.0, 1.0)]
    [double]$Gain = 0.25,

    [ValidateRange(20, 500)]
    [int]$PrebufferMilliseconds = 20,

    [ValidateSet('Safe', 'Balanced', 'Low')]
    [string]$LatencyMode = 'Low'
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator.'
}

if (-not ('SpatialAudioLabBattlefieldWindow' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class SpatialAudioLabBattlefieldWindow
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
        bool closed = false;
        EnumWindows(delegate (IntPtr window, IntPtr parameter) {
            uint ownerProcessId;
            GetWindowThreadProcessId(window, out ownerProcessId);
            if (ownerProcessId != (uint)processId) return true;

            StringBuilder text = new StringBuilder(256);
            GetWindowText(window, text, text.Capacity);
            if (!String.Equals(text.ToString(), title, StringComparison.Ordinal)) return true;

            closed = PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
            return false;
        }, IntPtr.Zero);
        return closed;
    }
}
'@
}

$shortcut = 'C:\Users\Public\Desktop\Battlefield 1.lnk'
if (-not (Test-Path -LiteralPath $shortcut)) {
    throw "Battlefield 1 shortcut was not found: $shortcut"
}
if (Get-Process bf1 -ErrorAction SilentlyContinue) {
    throw 'Battlefield 1 is already running.'
}

& (Join-Path $PSScriptRoot 'Start-LiveNativeMat714.ps1') `
    -Gain $Gain `
    -PrebufferMilliseconds $PrebufferMilliseconds `
    -LatencyMode $LatencyMode

Start-Process -FilePath $shortcut | Out-Null
$processDeadline = (Get-Date).AddSeconds(30)
do {
    Start-Sleep -Milliseconds 250
    $battlefield = Get-Process bf1 -ErrorAction SilentlyContinue
} while ($null -eq $battlefield -and (Get-Date) -lt $processDeadline)
if ($null -eq $battlefield) {
    throw 'Battlefield 1 did not start within 30 seconds.'
}

$dialogDeadline = (Get-Date).AddSeconds(20)
$dialogClosed = $false
do {
    $dialogClosed = [SpatialAudioLabBattlefieldWindow]::CloseWindow(
        $battlefield.Id, 'Minimum Hardware Check')
    if (-not $dialogClosed) { Start-Sleep -Milliseconds 200 }
} while (-not $dialogClosed -and (Get-Date) -lt $dialogDeadline)

if ($dialogClosed) {
    Write-Host 'Closed Minimum Hardware Check with WM_CLOSE; Battlefield remains running.'
} else {
    Write-Warning 'Minimum Hardware Check did not appear within 20 seconds.'
}
Write-Host "Battlefield 1 PID=$($battlefield.Id); native MAT remains active."
Write-Host 'Stop MAT later with: .\tools\Stop-LiveNativeMat714.ps1'
