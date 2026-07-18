[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Atmos', 'NativeMat', 'DtsX', 'Pcm')]
    [string]$Mode,

    [string]$EndpointFilter = '1 - HISENSE (Virtual Audio Device',

    [string]$DeviceInstanceId = 'ROOT\MEDIA\0001',

    [ValidateRange(5, 30)]
    [int]$TimeoutSeconds = 15
)

$repoRoot = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repoRoot 'build\SpatialAudioLab.CLI.exe'
$preflight = Join-Path $PSScriptRoot 'Test-SpatialProvider.ps1'
$atmosFormat = [Guid]'A289735D-FA3E-4E35-9D7D-B6F896ACB2E7'
$dtsXFormat = [Guid]'10201B4A-3322-4967-BF40-2CAA9BAFCA44'

if (-not (Test-Path $exe)) {
    throw "SpatialAudioLab.CLI.exe was not found: $exe"
}

Add-Type -AssemblyName System.Runtime.WindowsRuntime
[Windows.Media.Audio.SpatialAudioDeviceConfiguration,Windows.Media.Audio,ContentType=WindowsRuntime] |
    Out-Null
[Windows.Media.Audio.SetDefaultSpatialAudioFormatResult,Windows.Media.Audio,ContentType=WindowsRuntime] |
    Out-Null
[Windows.Media.Devices.MediaDevice,Windows.Media.Devices,ContentType=WindowsRuntime] | Out-Null

if (-not ('SpatialRegistryAccess' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class SpatialRegistryAccess {
    static readonly IntPtr HKEY_LOCAL_MACHINE =
        new IntPtr(unchecked((long)0x80000002));

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    static extern int RegOpenKeyEx(IntPtr key, string subKey, uint options,
                                    uint desiredAccess, out IntPtr result);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    static extern int RegQueryValueEx(IntPtr key, string name, IntPtr reserved,
                                       out uint type, byte[] data, ref uint size);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    static extern int RegSetValueEx(IntPtr key, string name, uint reserved,
                                     uint type, byte[] data, int size);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    static extern int RegDeleteValue(IntPtr key, string name);

    [DllImport("advapi32.dll")]
    static extern int RegCloseKey(IntPtr key);

    public static byte[] GetBinary(string subKey, string name) {
        IntPtr key;
        int error = RegOpenKeyEx(HKEY_LOCAL_MACHINE, subKey, 0, 0x0001, out key);
        if (error != 0) throw new Win32Exception(error);
        try {
            uint type;
            uint size = 0;
            error = RegQueryValueEx(key, name, IntPtr.Zero, out type, null, ref size);
            if (error == 2) return null;
            if (error != 0) throw new Win32Exception(error);
            if (type != 3) throw new InvalidOperationException(
                "The spatial endpoint property is not REG_BINARY.");
            byte[] data = new byte[size];
            error = RegQueryValueEx(key, name, IntPtr.Zero, out type, data, ref size);
            if (error != 0) throw new Win32Exception(error);
            return data;
        } finally {
            RegCloseKey(key);
        }
    }

    public static void SetBinary(string subKey, string name, byte[] data) {
        IntPtr key;
        int error = RegOpenKeyEx(HKEY_LOCAL_MACHINE, subKey, 0, 0x0002, out key);
        if (error != 0) throw new Win32Exception(error);
        try {
            if (data == null) {
                error = RegDeleteValue(key, name);
                if (error == 2) return;
            } else {
                error = RegSetValueEx(key, name, 0, 3, data, data.Length);
            }
            if (error != 0) throw new Win32Exception(error);
        } finally {
            RegCloseKey(key);
        }
    }

    public static byte[] FromHex(string hex) {
        if ((hex.Length & 1) != 0) throw new ArgumentException("Invalid hex string.");
        byte[] result = new byte[hex.Length / 2];
        for (int index = 0; index < result.Length; ++index)
            result[index] = Convert.ToByte(hex.Substring(index * 2, 2), 16);
        return result;
    }
}
'@
}

function Wait-WinRtOperation($Operation, [Type]$ResultType) {
    $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() |
        Where-Object {
            $_.Name -eq 'AsTask' -and $_.IsGenericMethodDefinition -and
            $_.GetParameters().Count -eq 1
        } | Select-Object -First 1
    if ($null -eq $asTask) {
        throw 'Could not resolve the WinRT AsTask adapter.'
    }
    $task = $asTask.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    $task.GetAwaiter().GetResult()
}

function Invoke-SpatialPreflight {
    param([string]$RequestedMode)

    try {
        $output = @(& $preflight -Mode $RequestedMode `
            -EndpointFilter $EndpointFilter 2>&1 | ForEach-Object { "$_" })
        return [pscustomobject]@{ Ready = $true; Output = $output }
    } catch {
        return [pscustomobject]@{ Ready = $false; Output = @("$_") }
    }
}

function Wait-SpatialPreflight {
    param([string]$RequestedMode)

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $result = $null
    do {
        $result = Invoke-SpatialPreflight $RequestedMode
        if ($result.Ready) { return $result }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    return $result
}

function Get-SpatialConfiguration {
    $deviceId = [Windows.Media.Devices.MediaDevice]::GetDefaultAudioRenderId(
        [Windows.Media.Devices.AudioDeviceRole]::Default)
    if ([string]::IsNullOrWhiteSpace($deviceId)) {
        throw 'Windows did not return a default render endpoint.'
    }
    $configuration = [Windows.Media.Audio.SpatialAudioDeviceConfiguration]::GetForDeviceId(
        $deviceId)
    if ($null -eq $configuration -or -not $configuration.IsSpatialAudioSupported) {
        throw "The default endpoint does not expose spatial audio: $deviceId"
    }
    [pscustomobject]@{ DeviceId = $deviceId; Configuration = $configuration }
}

function Set-WindowsSpatialFormat([Guid]$Format) {
    $spatial = Get-SpatialConfiguration
    $operation = $spatial.Configuration.SetDefaultSpatialAudioFormatAsync($Format)
    Wait-WinRtOperation $operation `
        ([Windows.Media.Audio.SetDefaultSpatialAudioFormatResult])
}

function Get-EndpointRegistrySubKey([string]$DeviceId) {
    if ($DeviceId -notmatch '(?i)\{0\.0\.0\.00000000\}\.\{(?<Endpoint>[0-9a-f-]{36})\}') {
        throw "Could not extract the endpoint GUID from: $DeviceId"
    }
    "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\" +
        "{$($Matches.Endpoint)}\Properties"
}

function Set-Bytes([byte[]]$Destination, [int]$Offset, [byte[]]$Source) {
    [Array]::Copy($Source, 0, $Destination, $Offset, $Source.Length)
}

function Get-SpatialRegistryTemplate([string]$RequestedMode) {
    $guid = if ($RequestedMode -eq 'Atmos') {
        '5D7389A23EFA354E9D7DB6F896ACB2E7'
    } else {
        '4A1B201022336749BF402CAA9BAFCA44'
    }
    $objectState = if ($RequestedMode -eq 'Atmos') {
        'FE1F0C000100000014000000'
    } else {
        'FEFF0F000100000020000000'
    }
    $carrierSubtype = if ($RequestedMode -eq 'Atmos') { '0C030000' } else { '0B010000' }

    [pscustomobject]@{
        Enabled = [SpatialRegistryAccess]::FromHex('02000000010000000100')
        Active = [SpatialRegistryAccess]::FromHex(
            '41000000010000000200005A' + $guid + $objectState +
            'A0400000A0400000A0400000000000000000000000000000F0410000' +
            'B442000007430000344200003442000007430000803F0000803F0000' +
            '803F0000803F0000803F0000803F0000803F0000803F96433B3FBF' +
            '0E1C3F0000803FBF0E1C3FD39FFD3ED39FFD3E0000803F00000000' +
            '00000000')
        Provider = [SpatialRegistryAccess]::FromHex(
            '41000000010000000200005A010000000000000001000000' +
            $guid + $guid +
            '00000000000000000000000000000000010000000000000001000000')
        Selection = [SpatialRegistryAccess]::FromHex(
            '41000000010000000000000000000000' + $guid)
        Carrier = [SpatialRegistryAccess]::FromHex(
            '4100000001000000FEFF080000EE020000E02E001000100016001000' +
            '3F060000' + $carrierSubtype + 'EA0C1000800000AA00389B71')
    }
}

function Write-NativePcmRegistryState(
        [string]$RegistrySubKey,
        [string[]]$ValueNames,
        [hashtable]$Original) {
    [SpatialRegistryAccess]::SetBinary($RegistrySubKey, $ValueNames[0],
        [SpatialRegistryAccess]::FromHex('02000000010000000000'))
    [SpatialRegistryAccess]::SetBinary($RegistrySubKey, $ValueNames[1],
        [SpatialRegistryAccess]::FromHex('410000000100000000'))

    $providerBytes = [Math]::Max(88, $Original[$ValueNames[2]].Length)
    $providerState = New-Object byte[] $providerBytes
    Set-Bytes $providerState 0 `
        ([SpatialRegistryAccess]::FromHex('41000000010000000200005A'))
    [SpatialRegistryAccess]::SetBinary($RegistrySubKey, $ValueNames[2], $providerState)

    $selection = New-Object byte[] 32
    Set-Bytes $selection 0 `
        ([SpatialRegistryAccess]::FromHex('41000000010000000000000000000000'))
    [SpatialRegistryAccess]::SetBinary($RegistrySubKey, $ValueNames[3], $selection)
}

function Set-NativePcmDeviceFormat {
    $output = @(& $exe set-pcm714-format $EndpointFilter 2>&1 |
        ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) {
        throw "Could not negotiate native PCM 7.1.4.`n$($output -join "`n")"
    }
    $output | ForEach-Object { if ($_ -ne '') { Write-Host $_ } }
}

function Set-SpatialCodecDeviceFormat([string]$RequestedMode) {
    $codec = if ($RequestedMode -eq 'NativeMat') {
        'mat10'
    } elseif ($RequestedMode -eq 'Atmos') {
        'atmos'
    } else {
        'dtsx'
    }
    $output = @(& $exe set-codec-format $codec $EndpointFilter 2>&1 |
        ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) {
        throw "Could not negotiate the $RequestedMode device and mix formats.`n" +
            ($output -join "`n")
    }
    $output | ForEach-Object { if ($_ -ne '') { Write-Host $_ } }
}

function Get-SpatialRegistryMode(
        [hashtable]$Values,
        [string[]]$ValueNames) {
    $enabled = $Values[$ValueNames[0]]
    if ($enabled.Length -lt 9 -or $enabled[8] -eq 0) { return 'Pcm' }

    $carrier = $Values[$ValueNames[4]]
    if ($carrier.Length -lt 36) { return $null }
    switch ([BitConverter]::ToUInt32($carrier, 32)) {
        0x030C { return 'Atmos' }
        0x010B { return 'DtsX' }
        default { return $null }
    }
}

function Write-SpatialRegistryState(
        [string]$RegistrySubKey,
        [string[]]$ValueNames,
        [string]$RequestedMode) {
    $values = Get-SpatialRegistryTemplate $RequestedMode
    [SpatialRegistryAccess]::SetBinary($RegistrySubKey, $ValueNames[0], $values.Enabled)
    [SpatialRegistryAccess]::SetBinary($RegistrySubKey, $ValueNames[1], $values.Active)
    [SpatialRegistryAccess]::SetBinary($RegistrySubKey, $ValueNames[2], $values.Provider)
    [SpatialRegistryAccess]::SetBinary($RegistrySubKey, $ValueNames[3], $values.Selection)
    [SpatialRegistryAccess]::SetBinary($RegistrySubKey, $ValueNames[4], $values.Carrier)
}

function Wait-SpatialEndpoint {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        & $exe set-default $EndpointFilter *> $null
        if ($LASTEXITCODE -eq 0) { return }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    throw "The endpoint '$EndpointFilter' did not return."
}

function Restart-AudioServices {
    Write-Host 'Restarting the Windows audio services.'
    Stop-Service Audiosrv -Force -ErrorAction Stop
    Stop-Service AudioEndpointBuilder -Force -ErrorAction Stop
    Start-Service AudioEndpointBuilder -ErrorAction Stop
    Start-Service Audiosrv -ErrorAction Stop
    Wait-SpatialEndpoint
}

function Restart-SpatialDevice {
    $restartOutput = @(& pnputil.exe /restart-device $DeviceInstanceId 2>&1 |
        ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) {
        throw "Could not restart $DeviceInstanceId.`n$($restartOutput -join "`n")"
    }
    $restartOutput | ForEach-Object { if ($_ -ne '') { Write-Host $_ } }

    Wait-SpatialEndpoint
}

function Set-SpatialRegistryFallback([string]$RequestedMode) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run PowerShell as Administrator to reconfigure the $RequestedMode provider."
    }

    $spatial = Get-SpatialConfiguration
    $registrySubKey = Get-EndpointRegistrySubKey $spatial.DeviceId
    $valueNames = @(
        '{6737016f-5360-48ee-af05-e616c8ff27a7},2',
        '{fd8a7b27-0b18-4025-ab1c-bdd6b32e1604},2',
        '{908dba32-edff-4c28-8e45-c918561f6748},2',
        '{8a845654-d6c3-4cd7-b4eb-243d4bd99032},2',
        '{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0'
    )
    $original = @{}
    foreach ($name in $valueNames) {
        $original[$name] = [SpatialRegistryAccess]::GetBinary($registrySubKey, $name)
    }
    $originalMode = Get-SpatialRegistryMode $original $valueNames

    try {
        if ($RequestedMode -eq 'Pcm') {
            Write-NativePcmRegistryState $registrySubKey $valueNames $original
            Write-Host 'Rebuilding the audio services for native PCM 7.1.4.'
            Restart-AudioServices
            Set-NativePcmDeviceFormat
        } elseif ($RequestedMode -eq 'NativeMat') {
            Write-Host 'Selecting the MAT carrier before disabling Windows spatial audio.'
            Set-SpatialCodecDeviceFormat $RequestedMode
            Write-NativePcmRegistryState $registrySubKey $valueNames $original
            Write-Host 'Rebuilding the audio services for native MAT.'
            Restart-AudioServices
        } else {
            Write-Host "Disabling the current spatial provider before selecting $RequestedMode."
            Write-NativePcmRegistryState $registrySubKey $valueNames $original
            Restart-AudioServices
            Set-SpatialCodecDeviceFormat $RequestedMode
            Write-SpatialRegistryState $registrySubKey $valueNames $RequestedMode
            Write-Host "Rebuilding the audio services for $RequestedMode."
            Restart-AudioServices
        }

        $verified = Wait-SpatialPreflight $RequestedMode
        if (-not $verified.Ready) {
            Write-Warning 'The service restart was insufficient; trying device reenumeration.'
            if ($RequestedMode -in @('Pcm', 'NativeMat')) {
                Write-NativePcmRegistryState $registrySubKey $valueNames $original
            } else {
                Write-SpatialRegistryState $registrySubKey $valueNames $RequestedMode
            }
            if ($RequestedMode -eq 'NativeMat') {
                Set-SpatialCodecDeviceFormat $RequestedMode
                Write-NativePcmRegistryState $registrySubKey $valueNames $original
            }
            Restart-SpatialDevice
            if ($RequestedMode -eq 'Pcm') {
                Set-NativePcmDeviceFormat
            }
            $verified = Wait-SpatialPreflight $RequestedMode
            if (-not $verified.Ready) {
                throw "$RequestedMode did not become ready.`n$($verified.Output -join "`n")"
            }
        }
        $verified.Output | Write-Host
    } catch {
        $failure = "$_"
        Write-Warning "$RequestedMode activation failed; restoring the previous spatial provider state."
        foreach ($name in $valueNames) {
            [SpatialRegistryAccess]::SetBinary($registrySubKey, $name, $original[$name])
        }
        try {
            Restart-AudioServices
            if ($originalMode -eq 'Pcm') { Set-NativePcmDeviceFormat }
        } catch {
            try { Restart-SpatialDevice } catch { Write-Warning "Rollback restart failed: $_" }
        }
        throw $failure
    }
}

& $exe set-default $EndpointFilter
if ($LASTEXITCODE -ne 0) {
    throw "Could not select '$EndpointFilter' as the default endpoint: $LASTEXITCODE"
}

$alreadyReady = Invoke-SpatialPreflight $Mode
if ($alreadyReady.Ready) {
    $alreadyReady.Output | Write-Host
    if ($Mode -eq 'NativeMat') {
        Write-Host 'Native MAT mode is already active.'
    } elseif ($Mode -eq 'Pcm') {
        Write-Host 'Native PCM 7.1.4 mode is already active.'
    } else {
        Write-Host "$Mode spatial provider is already active."
    }
    return
}

if ($Mode -in @('Pcm', 'NativeMat')) {
    Set-SpatialRegistryFallback $Mode
    if ($Mode -eq 'Pcm') {
        Write-Host 'Native PCM 7.1.4 mode selected and verified.'
    } else {
        Write-Host 'Native MAT mode selected and verified.'
    }
    return
}

$targetFormat = if ($Mode -eq 'Atmos') { $atmosFormat } else { $dtsXFormat }
$result = $null
try {
    $result = Set-WindowsSpatialFormat $targetFormat
    Write-Host "Windows spatial format request: $($result.Status)"
} catch {
    Write-Host "Windows spatial format request failed: $_"
}

if ($null -ne $result -and $result.Status -eq 'Succeeded') {
    $verified = Wait-SpatialPreflight $Mode
    if ($verified.Ready) {
        $verified.Output | Write-Host
        Write-Host "$Mode spatial provider selected and verified."
        return
    }
}

Set-SpatialRegistryFallback $Mode
Write-Host "$Mode spatial provider selected and verified."
