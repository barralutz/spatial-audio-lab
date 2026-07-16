[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Apply", "Remove")]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [string]$InstanceId,

    [string]$EdidFile,

    [switch]$SkipRestart
)

$ErrorActionPreference = "Stop"

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-BlockChecksum {
    param([byte[]]$Bytes, [int]$Offset)
    $sum = 0
    for ($i = 0; $i -lt 128; $i++) {
        $sum = ($sum + $Bytes[$Offset + $i]) -band 0xff
    }
    return $sum -eq 0
}

if (-not (Test-Administrator)) {
    throw "Ejecuta este script desde PowerShell como administrador."
}

$deviceParameters = "HKLM:\SYSTEM\CurrentControlSet\Enum\$InstanceId\Device Parameters"
if (-not (Test-Path -LiteralPath $deviceParameters)) {
    throw "No existe la instancia de monitor: $InstanceId"
}

$safeInstance = $InstanceId -replace '[^A-Za-z0-9._-]', '_'
$backupDirectory = Join-Path $PSScriptRoot "..\generated\registry-backup"
New-Item -ItemType Directory -Force -Path $backupDirectory | Out-Null
$backupPath = Join-Path $backupDirectory "$safeInstance-before-override.reg"
$nativeRegistryPath = "HKLM\SYSTEM\CurrentControlSet\Enum\$InstanceId\Device Parameters"

if (-not (Test-Path -LiteralPath $backupPath)) {
    & reg.exe export $nativeRegistryPath $backupPath /y | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "No se pudo respaldar Device Parameters (reg.exe=$LASTEXITCODE)."
    }
    Write-Host "Backup: $backupPath"
}

$overridePath = Join-Path $deviceParameters "EDID_OVERRIDE"

if ($Action -eq "Remove") {
    if (Test-Path -LiteralPath $overridePath) {
        if ($PSCmdlet.ShouldProcess($overridePath, "Eliminar override EDID")) {
            Remove-Item -LiteralPath $overridePath -Recurse -Force
            Write-Host "Override eliminado de $InstanceId"
        }
    } else {
        Write-Host "La instancia no tiene EDID_OVERRIDE."
    }
} else {
    if (-not $EdidFile) { throw "EdidFile es obligatorio para Action=Apply." }
    $resolvedEdid = (Resolve-Path -LiteralPath $EdidFile).Path
    $edid = [IO.File]::ReadAllBytes($resolvedEdid)

    if ($edid.Length -lt 128 -or ($edid.Length % 128) -ne 0) {
        throw "El EDID debe contener bloques completos de 128 bytes."
    }
    $expectedBlocks = 1 + [int]$edid[126]
    if ($expectedBlocks -gt ($edid.Length / 128)) {
        throw "El bloque base anuncia $expectedBlocks bloques pero el archivo solo contiene $($edid.Length / 128)."
    }
    for ($blockIndex = 0; $blockIndex -lt $expectedBlocks; $blockIndex++) {
        if (-not (Test-BlockChecksum $edid ($blockIndex * 128))) {
            throw "Checksum invalido en el bloque EDID $blockIndex."
        }
    }

    if ($PSCmdlet.ShouldProcess($overridePath, "Instalar $expectedBlocks bloques EDID")) {
        if (Test-Path -LiteralPath $overridePath) {
            Remove-Item -LiteralPath $overridePath -Recurse -Force
        }
        New-Item -Path $overridePath | Out-Null

        for ($blockIndex = 0; $blockIndex -lt $expectedBlocks; $blockIndex++) {
            $block = New-Object byte[] 128
            [Array]::Copy($edid, $blockIndex * 128, $block, 0, 128)
            New-ItemProperty -LiteralPath $overridePath -Name "$blockIndex" `
                -PropertyType Binary -Value $block -Force | Out-Null
        }
        Write-Host "Override aplicado a $InstanceId desde $resolvedEdid"
    }
}

if (-not $SkipRestart) {
    Write-Host "Reiniciando la instancia de monitor..."
    & pnputil.exe /restart-device $InstanceId
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "pnputil no pudo reiniciar el monitor. Reinicia Windows para aplicar el cambio."
    }
}
