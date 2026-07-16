[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$WithApoExtensions,
    [switch]$EnableDiagnosticFileCapture
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator. Driver installation requires elevation.'
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $repoRoot `
    "driver\windows-driver-samples\audio\sysvad\Package\x64\$Configuration"
$package = Join-Path $buildRoot 'package'
$certificatePath = Join-Path $buildRoot 'package.cer'
$devcon = Join-Path ${env:ProgramFiles(x86)} `
    'Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe'
$hardwareId = 'Root\sysvad_ComponentizedAudioSample'

$requiredFiles = @(
    $certificatePath
    $devcon
    (Join-Path $package 'ComponentizedAudioSample.inf')
    (Join-Path $package 'TabletAudioSample.sys')
    (Join-Path $package 'KeywordDetectorContosoAdapter.dll')
    (Join-Path $package 'sysvad.cat')
)
if ($WithApoExtensions) {
    $requiredFiles += @(
        (Join-Path $package 'ComponentizedApoSample.inf')
        (Join-Path $package 'ComponentizedAudioSampleExtension.inf')
        (Join-Path $package 'SwapApo.dll')
        (Join-Path $package 'DelayApo.dll')
        (Join-Path $package 'KwsApo.dll')
        (Join-Path $package 'AecApo.dll')
    )
}

$missingFiles = @($requiredFiles | Where-Object { -not (Test-Path $_) })
if ($missingFiles.Count -ne 0) {
    throw "The SysVAD package is incomplete. Missing: $($missingFiles -join ', ')"
}

$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
foreach ($storeName in 'Root', 'TrustedPublisher') {
    $storePath = "Cert:\LocalMachine\$storeName\$($certificate.Thumbprint)"
    if (Test-Path $storePath) {
        Write-Host "Test certificate is already present in LocalMachine\$storeName."
    } else {
        Write-Host "Adding the WDK test certificate to LocalMachine\$storeName..."
        $imported = Import-Certificate -FilePath $certificatePath `
            -CertStoreLocation "Cert:\LocalMachine\$storeName"
        if (-not $imported) {
            throw "Failed to import the test certificate into LocalMachine\$storeName."
        }
    }
}

$existingDevices = @(Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object {
    $hardwareIds = (Get-PnpDeviceProperty -InstanceId $_.InstanceId `
        -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue).Data
    $hardwareIds -contains $hardwareId
})
if ($existingDevices.Count -eq 0) {
    Write-Host 'Creating and installing the SysVAD root audio device...'
    Push-Location $package
    try {
        & $devcon install 'ComponentizedAudioSample.inf' $hardwareId
        $devconExitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($devconExitCode -notin 0, 1) {
        throw "DevCon failed to install the SysVAD device: $devconExitCode"
    }
    if ($devconExitCode -eq 1) {
        Write-Warning 'DevCon reports that Windows needs a restart before the device can be used.'
    }
} else {
    Write-Host 'Updating the existing SysVAD root audio device...'
    & pnputil.exe /add-driver (Join-Path $package 'ComponentizedAudioSample.inf') /install
    $updateExitCode = $LASTEXITCODE
    if ($updateExitCode -notin 0, 259, 3010) {
        throw "PnPUtil failed to update ComponentizedAudioSample.inf: $updateExitCode"
    }
    if ($updateExitCode -eq 3010) {
        Write-Warning 'Windows staged the new base driver and reports that a reboot may be required.'
    } elseif ($updateExitCode -eq 259) {
        Write-Warning 'PnPUtil selected the staged driver but did not complete the live replacement.'
    }

    foreach ($device in $existingDevices) {
        & pnputil.exe /restart-device $device.InstanceId
        if ($LASTEXITCODE -notin 0, 3010) {
            Write-Warning "PnPUtil could not restart $($device.InstanceId): $LASTEXITCODE"
        } elseif ($LASTEXITCODE -eq 3010) {
            Write-Warning "Windows still requires a reboot to update $($device.InstanceId)."
        }
    }
}

$parametersPath = 'HKLM:\SYSTEM\CurrentControlSet\Services\sysvad_componentizedaudiosample\Parameters'
New-Item -Path $parametersPath -Force | Out-Null
$disableDataFiles = if ($EnableDiagnosticFileCapture) { 0 } else { 1 }
New-ItemProperty -Path $parametersPath -Name 'DoNotCreateDataFiles' -PropertyType DWord `
    -Value $disableDataFiles -Force | Out-Null
Write-Host "DoNotCreateDataFiles=$disableDataFiles (the live MAT ring remains enabled)."

if ($WithApoExtensions) {
    foreach ($infName in 'ComponentizedApoSample.inf', 'ComponentizedAudioSampleExtension.inf') {
        Write-Host "Staging $infName..."
        & pnputil.exe /add-driver (Join-Path $package $infName) /install
        if ($LASTEXITCODE -ne 0) {
            throw "PnPUtil failed to install $infName`: $LASTEXITCODE"
        }
    }
}

Start-Sleep -Seconds 2
Write-Host ''
Write-Host 'Installed SysVAD devices:'
Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
    $_.InstanceId -in $existingDevices.InstanceId -or
    $_.FriendlyName -like '*SYSVAD*' -or
    $_.FriendlyName -like '*Virtual Audio Device*Tablet Sample*'
} | Sort-Object Class, FriendlyName | Format-Table Status, Class, FriendlyName, InstanceId -AutoSize

Write-Warning 'This is a development certificate and test driver. Do not run Battlefield or other anti-cheat games in this boot.'
Write-Host 'Use tools\Uninstall-SysvadCapture.ps1 when the capture test is complete.'
