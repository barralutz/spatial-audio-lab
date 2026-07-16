[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputFile
)

$ErrorActionPreference = "Stop"
$path = (Resolve-Path -LiteralPath $InputFile).Path
$edid = [IO.File]::ReadAllBytes($path)

if (($edid.Length % 128) -ne 0) { throw "Longitud EDID invalida: $($edid.Length)." }

$formatNames = @{
    1 = "LPCM"
    2 = "AC-3"
    7 = "DTS"
    10 = "E-AC-3"
    11 = "DTS-HD"
    12 = "MAT/MLP/TrueHD"
}
$rateBits = @(
    @{ Mask = 0x01; Name = "32" },
    @{ Mask = 0x02; Name = "44.1" },
    @{ Mask = 0x04; Name = "48" },
    @{ Mask = 0x08; Name = "88.2" },
    @{ Mask = 0x10; Name = "96" },
    @{ Mask = 0x20; Name = "176.4" },
    @{ Mask = 0x40; Name = "192" }
)

Write-Host "Archivo: $path"
Write-Host "Bloques: $($edid.Length / 128)"

for ($blockIndex = 0; $blockIndex -lt ($edid.Length / 128); $blockIndex++) {
    $offset = $blockIndex * 128
    $sum = 0
    for ($i = 0; $i -lt 128; $i++) { $sum = ($sum + $edid[$offset + $i]) -band 0xff }
    Write-Host "Bloque $blockIndex checksum: $(if ($sum -eq 0) { 'OK' } else { "ERROR ($sum)" })"

    if ($blockIndex -eq 0 -or $edid[$offset] -ne 0x02) { continue }

    $dtdOffset = [int]$edid[$offset + 2]
    $flags = [int]$edid[$offset + 3]
    Write-Host ("  CTA revision: {0}, DTD offset: {1}, basic audio: {2}" -f `
        $edid[$offset + 1], $dtdOffset, [bool]($flags -band 0x40))
    if ($dtdOffset -eq 0) { continue }

    $position = 4
    while ($position -lt $dtdOffset) {
        $header = [int]$edid[$offset + $position]
        if ($header -eq 0) { break }
        $tag = $header -shr 5
        $length = $header -band 0x1f

        if ($tag -eq 1) {
            if (($length % 3) -ne 0) { throw "Audio Data Block truncado." }
            Write-Host "  Audio Data Block ($length bytes):"
            for ($sad = 0; $sad -lt $length; $sad += 3) {
                $byte1 = [int]$edid[$offset + $position + 1 + $sad]
                $byte2 = [int]$edid[$offset + $position + 2 + $sad]
                $byte3 = [int]$edid[$offset + $position + 3 + $sad]
                $format = ($byte1 -band 0x78) -shr 3
                $channels = ($byte1 -band 0x07) + 1
                $name = $formatNames[$format]
                if (-not $name) { $name = "format-$format" }
                $rates = @($rateBits | Where-Object { $byte2 -band $_.Mask } | ForEach-Object { $_.Name })
                $detail = ""
                if ($format -eq 1) {
                    $depths = @()
                    if ($byte3 -band 0x01) { $depths += "16" }
                    if ($byte3 -band 0x02) { $depths += "20" }
                    if ($byte3 -band 0x04) { $depths += "24" }
                    $detail = "; bits=$($depths -join '/')"
                } elseif ($format -eq 10) {
                    $detail = "; JOC=$([bool]($byte3 -band 0x01)); ACMOD28=$([bool]($byte3 -band 0x02))"
                } elseif ($format -eq 12) {
                    $detail = "; object/channel PCM=$([bool]($byte3 -band 0x01)); hash-not-required=$([bool]($byte3 -band 0x02))"
                }
                Write-Host ("    {0}: {1}ch; {2} kHz{3}; SAD={4:X2} {5:X2} {6:X2}" -f `
                    $name, $channels, ($rates -join '/'), $detail, $byte1, $byte2, $byte3)
            }
        } elseif ($tag -eq 4 -and $length -ge 3) {
            Write-Host ("  Speaker Allocation: {0:X2} {1:X2} {2:X2}" -f `
                $edid[$offset + $position + 1],
                $edid[$offset + $position + 2],
                $edid[$offset + $position + 3])
        }
        $position += $length + 1
    }
}
