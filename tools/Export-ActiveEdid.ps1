[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$InstanceId
)

$ErrorActionPreference = "Stop"

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $PSScriptRoot "..\generated\edid-backup"
}

function Get-SafeName {
    param([string]$Value)
    return ($Value -replace '[^A-Za-z0-9._-]', '_')
}

$monitors = Get-PnpDevice -Class Monitor |
    Where-Object { $_.Status -eq "OK" }

if ($InstanceId) {
    $monitors = $monitors | Where-Object { $_.InstanceId -ieq $InstanceId }
}

if (-not $monitors) {
    throw "No se encontro ningun monitor activo que coincida con InstanceId."
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$manifest = @()

foreach ($monitor in $monitors) {
    $deviceParameters = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($monitor.InstanceId)\Device Parameters"
    $edid = (Get-ItemProperty -LiteralPath $deviceParameters -Name EDID -ErrorAction Stop).EDID
    $hardwareIds = (Get-PnpDeviceProperty -InstanceId $monitor.InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data

    if (-not $edid -or ($edid.Length % 128) -ne 0) {
        Write-Warning "EDID invalido u omitido para $($monitor.InstanceId)."
        continue
    }

    $fileName = (Get-SafeName "$($monitor.InstanceId).bin")
    $filePath = Join-Path $OutputDirectory $fileName
    [IO.File]::WriteAllBytes($filePath, [byte[]]$edid)

    $manifest += [PSCustomObject]@{
        FriendlyName = $monitor.FriendlyName
        InstanceId   = $monitor.InstanceId
        HardwareIds  = @($hardwareIds)
        Bytes        = $edid.Length
        File         = (Resolve-Path $filePath).Path
    }

    Write-Host "Exportado: $($monitor.FriendlyName)"
    Write-Host "  InstanceId: $($monitor.InstanceId)"
    Write-Host "  HardwareId: $($hardwareIds -join ', ')"
    Write-Host "  Archivo:    $filePath"
}

$manifestPath = Join-Path $OutputDirectory "manifest.json"
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Manifest: $manifestPath"
