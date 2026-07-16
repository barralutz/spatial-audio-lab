[CmdletBinding()]
param()

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

$secureBoot = $null
try {
    $secureBoot = [bool](Confirm-SecureBootUEFI -ErrorAction Stop)
} catch {
    $state = Get-ItemProperty `
        'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' `
        -ErrorAction SilentlyContinue
    if ($null -ne $state) {
        $secureBoot = [bool]$state.UEFISecureBootEnabled
    }
}

$hvci = Get-ItemProperty `
    'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' `
    -ErrorAction SilentlyContinue

$bitLockerStatus = 'Requires an elevated PowerShell'
if ($isAdmin) {
    try {
        $volume = Get-BitLockerVolume -MountPoint 'C:' -ErrorAction Stop
        $bitLockerStatus = '{0}; protection {1}' -f `
            $volume.VolumeStatus, $volume.ProtectionStatus
    } catch {
        $bitLockerStatus = $_.Exception.Message
    }
}

$bcdTestSigning = 'Requires an elevated PowerShell'
if ($isAdmin) {
    $bcd = & "$env:SystemRoot\System32\bcdedit.exe" /enum '{current}' 2>&1
    $testLine = $bcd | Select-String -Pattern '^testsigning\s+' | Select-Object -First 1
    $bcdTestSigning = if ($testLine) { $testLine.Line.Trim() } else { 'Off/not present' }
}

$vsWhere = Join-Path ${env:ProgramFiles(x86)} `
    'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = if (Test-Path $vsWhere) {
    & $vsWhere -latest -products '*' -property installationPath
} else {
    'Not installed'
}

$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$wdkInstalled = Test-Path (Join-Path $sdkRoot 'build\10.0.26100.0\WindowsDriver.Common.targets')

[pscustomobject]@{
    Elevated              = $isAdmin
    SecureBoot            = $secureBoot
    BitLockerC            = $bitLockerStatus
    MemoryIntegrityHVCI   = if ($hvci) { [bool]$hvci.Enabled } else { $false }
    TestSigning           = $bcdTestSigning
    VisualStudio          = $visualStudio
    Wdk26100BuildTargets  = $wdkInstalled
} | Format-List

if (-not $isAdmin) {
    Write-Warning 'Run this script from PowerShell as Administrator for BitLocker and BCD status.'
}
