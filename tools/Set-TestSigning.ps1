[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Enable', 'Disable')]
    [string]$Action
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator.'
}

$bcdedit = "$env:SystemRoot\System32\bcdedit.exe"

if ($Action -eq 'Enable') {
    $secureBoot = $false
    try {
        $secureBoot = [bool](Confirm-SecureBootUEFI -ErrorAction Stop)
    } catch {
        $state = Get-ItemProperty `
            'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' `
            -ErrorAction SilentlyContinue
        $secureBoot = $null -ne $state -and [bool]$state.UEFISecureBootEnabled
    }

    if ($secureBoot) {
        throw @'
Secure Boot is enabled. Windows will reject TESTSIGNING changes.
Back up the BitLocker recovery key, suspend BitLocker, and disable Secure Boot in UEFI first.
For the first driver test, prefer Advanced startup option 7 instead of persistent Test Mode.
'@
    }

    try {
        $volume = Get-BitLockerVolume -MountPoint 'C:' -ErrorAction Stop
        if ($volume.ProtectionStatus -eq 'On') {
            throw @'
BitLocker protection is active on C:. Do not change boot policy yet.
Save the recovery key and suspend protection from Control Panel or with Suspend-BitLocker.
'@
        }
    } catch {
        if ($_.Exception.Message -match 'BitLocker protection is active') { throw }
        Write-Warning "Could not determine BitLocker state: $($_.Exception.Message)"
    }

    if ($PSCmdlet.ShouldProcess('current Windows boot entry', 'Enable TESTSIGNING')) {
        & $bcdedit /set testsigning on
        if ($LASTEXITCODE -ne 0) { throw "BCDEdit failed with exit code $LASTEXITCODE" }
        Write-Host 'TESTSIGNING enabled. Restart Windows to enter Test Mode.'
    }
} else {
    if ($PSCmdlet.ShouldProcess('current Windows boot entry', 'Disable TESTSIGNING')) {
        & $bcdedit /set testsigning off
        if ($LASTEXITCODE -ne 0) { throw "BCDEdit failed with exit code $LASTEXITCODE" }
        Write-Host 'TESTSIGNING disabled. Restart, re-enable Secure Boot, then resume BitLocker.'
    }
}
