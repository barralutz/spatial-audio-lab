[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputFile,

    [string]$OutputDirectory,

    [string]$HardwareId = "MONITOR\SAM0F70"
)

$ErrorActionPreference = "Stop"

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $PSScriptRoot "..\generated\mat-override"
}

function Test-EdidBlockChecksum {
    param([byte[]]$Bytes, [int]$Offset)
    $sum = 0
    for ($i = 0; $i -lt 128; $i++) {
        $sum = ($sum + $Bytes[$Offset + $i]) -band 0xff
    }
    return $sum -eq 0
}

function Get-CtaBlocks {
    param([byte[]]$Block)

    $dtdOffset = [int]$Block[2]
    if ($dtdOffset -eq 0) { return @() }
    if ($dtdOffset -lt 4 -or $dtdOffset -gt 127) {
        throw "Offset DTD CTA invalido: $dtdOffset"
    }

    $result = @()
    $position = 4
    while ($position -lt $dtdOffset) {
        $header = [int]$Block[$position]
        if ($header -eq 0) { break }
        $length = $header -band 0x1f
        $end = $position + $length
        if ($end -ge $dtdOffset) {
            throw "Data Block CTA truncado en offset $position."
        }
        $bytes = New-Object byte[] ($length + 1)
        [Array]::Copy($Block, $position, $bytes, 0, $bytes.Length)
        $result += ,$bytes
        $position += $bytes.Length
    }
    return $result
}

function Get-CtaDetailedTimings {
    param([byte[]]$Block)

    $result = @()
    $position = [int]$Block[2]
    if ($position -eq 0) { return $result }

    while (($position + 18) -le 127) {
        $descriptor = New-Object byte[] 18
        [Array]::Copy($Block, $position, $descriptor, 0, 18)
        if (($descriptor | Where-Object { $_ -ne 0 }).Count -eq 0) { break }
        $result += ,$descriptor
        $position += 18
    }
    return $result
}

function New-DataBlock {
    param([int]$Tag, [byte[]]$Payload)

    if ($Payload.Length -gt 31) { throw "Payload CTA demasiado grande." }
    $block = New-Object byte[] ($Payload.Length + 1)
    $block[0] = [byte](($Tag -shl 5) -bor $Payload.Length)
    [Array]::Copy($Payload, 0, $block, 1, $Payload.Length)
    return $block
}

function Get-InfByteList {
    param([byte[]]$Block)
    return (($Block | ForEach-Object { "0x{0:x2}" -f $_ }) -join ',')
}

$inputPath = (Resolve-Path -LiteralPath $InputFile).Path
$edid = [IO.File]::ReadAllBytes($inputPath)

if ($edid.Length -lt 256 -or ($edid.Length % 128) -ne 0) {
    throw "El EDID debe contener al menos dos bloques completos de 128 bytes."
}

$expectedHeader = [byte[]](0x00,0xff,0xff,0xff,0xff,0xff,0xff,0x00)
for ($i = 0; $i -lt $expectedHeader.Length; $i++) {
    if ($edid[$i] -ne $expectedHeader[$i]) { throw "Cabecera EDID invalida." }
}

for ($offset = 0; $offset -lt $edid.Length; $offset += 128) {
    if (-not (Test-EdidBlockChecksum $edid $offset)) {
        throw "Checksum invalido en el bloque EDID $($offset / 128)."
    }
}

$ctaIndex = -1
for ($index = 1; $index -lt ($edid.Length / 128); $index++) {
    if ($edid[$index * 128] -eq 0x02) {
        $ctaIndex = $index
        break
    }
}
if ($ctaIndex -lt 0) { throw "No se encontro una extension CTA-861 para parchear." }

$cta = New-Object byte[] 128
[Array]::Copy($edid, $ctaIndex * 128, $cta, 0, 128)
$blocks = Get-CtaBlocks $cta
$detailedTimings = Get-CtaDetailedTimings $cta

# CTA SADs: LPCM 8ch, E-AC-3/JOC 8ch y MAT/MLP con object/channel PCM.
# 48/96/192 kHz; 16/20/24-bit para LPCM; MAT byte 3 = 0x03.
$audioPayload = [byte[]](
    0x0f,0x54,0x07,
    0x57,0x04,0x03,
    0x67,0x54,0x03
)
$audioBlock = New-DataBlock 1 $audioPayload
$speakerBlock = New-DataBlock 4 ([byte[]](0x4f,0x00,0x00))

$newBlocks = @()
$audioInserted = $false
$speakersInserted = $false
foreach ($block in $blocks) {
    $tag = ([int]$block[0]) -shr 5
    if ($tag -eq 1) {
        if (-not $audioInserted) {
            $newBlocks += ,$audioBlock
            $audioInserted = $true
        }
        continue
    }
    if ($tag -eq 4) {
        if (-not $speakersInserted) {
            $newBlocks += ,$speakerBlock
            $speakersInserted = $true
        }
        continue
    }
    $newBlocks += ,$block
}
if (-not $audioInserted) { $newBlocks += ,$audioBlock }
if (-not $speakersInserted) { $newBlocks += ,$speakerBlock }

$dataBlockBytes = ($newBlocks | ForEach-Object { $_.Length } | Measure-Object -Sum).Sum
if (-not $dataBlockBytes) { $dataBlockBytes = 0 }
$newDtdOffset = 4 + $dataBlockBytes
$requiredBytes = $newDtdOffset + (18 * $detailedTimings.Count)
if ($requiredBytes -gt 127) {
    throw "El CTA parcheado no cabe sin eliminar timings de video ($requiredBytes bytes)."
}

$patchedCta = New-Object byte[] 128
$patchedCta[0] = 0x02
$patchedCta[1] = $cta[1]
$patchedCta[2] = [byte]$newDtdOffset
$patchedCta[3] = [byte]($cta[3] -bor 0x40)

$position = 4
foreach ($block in $newBlocks) {
    [Array]::Copy($block, 0, $patchedCta, $position, $block.Length)
    $position += $block.Length
}
foreach ($descriptor in $detailedTimings) {
    [Array]::Copy($descriptor, 0, $patchedCta, $position, 18)
    $position += 18
}

$sum = 0
for ($i = 0; $i -lt 127; $i++) { $sum = ($sum + $patchedCta[$i]) -band 0xff }
$patchedCta[127] = [byte]((256 - $sum) -band 0xff)

$patchedEdid = New-Object byte[] $edid.Length
[Array]::Copy($edid, 0, $patchedEdid, 0, $edid.Length)
[Array]::Copy($patchedCta, 0, $patchedEdid, $ctaIndex * 128, 128)

if (-not (Test-EdidBlockChecksum $patchedEdid ($ctaIndex * 128))) {
    throw "Fallo interno: checksum CTA parcheado invalido."
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$baseName = [IO.Path]::GetFileNameWithoutExtension($inputPath)
$binPath = Join-Path $OutputDirectory "$baseName-mat.bin"
$infPath = Join-Path $OutputDirectory "$baseName-mat.inf"
[IO.File]::WriteAllBytes($binPath, $patchedEdid)

$hardwareModel = ($HardwareId -split '\\')[-1]
$infLines = @(
    '[Version]',
    'Signature="$WINDOWS NT$"',
    'Class=Monitor',
    'ClassGuid={4d36e96e-e325-11ce-bfc1-08002be10318}',
    'Provider=%ProviderName%',
    'DriverVer=07/15/2026,1.0.0.0',
    '',
    '[Manufacturer]',
    '%ProviderName%=Models,NTamd64',
    '',
    '[Models.NTamd64]',
    "%ModelName%=Install,$HardwareId",
    '',
    '[Install]',
    'DelReg=DeleteEdidOverride',
    'AddReg=EdidOverride',
    '',
    '[DeleteEdidOverride]',
    'HKR,EDID_OVERRIDE',
    '',
    '[EdidOverride]'
)

$block = New-Object byte[] 128
[Array]::Copy($patchedEdid, $ctaIndex * 128, $block, 0, 128)
$infLines += "HKR,EDID_OVERRIDE,`"$ctaIndex`",0x00000001,$(Get-InfByteList $block)"

$infLines += @(
    '',
    '[Strings]',
    'ProviderName="dolbyDecoder experimental"',
    "ModelName=`"$hardwareModel MAT experiment`""
)
$infLines | Set-Content -LiteralPath $infPath -Encoding ASCII

Write-Host "EDID original: $inputPath"
Write-Host "CTA parcheada: bloque $ctaIndex, DTD offset $($cta[2]) -> $newDtdOffset"
Write-Host "Audio SADs: LPCM 8ch / E-AC-3 JOC / MAT object PCM"
Write-Host "EDID generado: $binPath"
Write-Host "INF generado:  $infPath"
Write-Warning "No se ha instalado el override. Verifica el .bin y conserva el backup antes de aplicarlo."
