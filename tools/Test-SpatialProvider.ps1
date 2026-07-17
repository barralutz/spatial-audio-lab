[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Atmos', 'DtsX')]
    [string]$Mode,

    [string]$EndpointFilter = 'SinkDescription Sample'
)

$repoRoot = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repoRoot 'build\dolby-probe.exe'
if (-not (Test-Path $exe)) {
    throw "dolby-probe.exe was not found: $exe"
}

$formatOutput = @(& $exe device-format $EndpointFilter 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) {
    throw "Could not inspect the spatial endpoint.`n$($formatOutput -join "`n")"
}
$currentFormat = $formatOutput | Where-Object { $_ -match '^\s*device current:' } |
    Select-Object -First 1
$formatMatches = if ($Mode -eq 'Atmos') {
    $currentFormat -match 'Dolby (MLP / MAT 1\.0|MAT 2\.)'
} else {
    $currentFormat -match 'DTS:X E[12]'
}
if (-not $formatMatches) {
    $wanted = if ($Mode -eq 'Atmos') {
        'Dolby Atmos para el centro de entretenimiento'
    } else {
        'DTS:X para centro de entretenimiento'
    }
    throw @"
El carrier activo no corresponde a $Mode ($currentFormat).
El selector automatico debe activar '$wanted' antes de iniciar el puente.
"@
}

$spatialOutput = @(& $exe spatial-test 0.25 $EndpointFilter silence 2>&1 |
    ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) {
    throw @"
El carrier de $Mode esta seleccionado, pero el renderer espacial no puede abrir un stream.
$($spatialOutput -join "`n")
"@
}

$signature = $spatialOutput | Where-Object { $_ -match 'Native static mask:' } |
    Select-Object -First 1
$signatureMatches = if ($Mode -eq 'Atmos') {
    $signature -match '0xC1FFE, dynamic objects: 20'
} else {
    $signature -match '0xFFFFE, dynamic objects: 32'
}
if (-not $signatureMatches) {
    throw "El carrier esta activo, pero la firma espacial no corresponde a $Mode ($signature)."
}

Write-Host "$Mode spatial provider ready."
Write-Host $currentFormat
Write-Host $signature
