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
Deten los puentes y selecciona '$wanted' en Configuracion > Sonido > SinkDescription Sample.
Cambia tambien 'Sonido espacial'; no basta con cambiar solamente 'Formato'.
"@
}

$spatialOutput = @(& $exe spatial-test 0.25 $EndpointFilter silence 2>&1 |
    ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) {
    $repair = if ($Mode -eq 'Atmos') {
        " Si Dolby no aparece en Sonido espacial, ejecuta tools\Repair-DolbySpatialProvider.ps1."
    } else {
        ' Abre DTS Sound Unbound y vuelve a activar DTS:X Home Theater.'
    }
    throw @"
El carrier de $Mode esta seleccionado, pero el renderer espacial no puede abrir un stream.
En Configuracion > Sonido > SinkDescription Sample selecciona el mismo proveedor en Sonido espacial.$repair
$($spatialOutput -join "`n")
"@
}

if ($Mode -eq 'Atmos') {
    $signature = $spatialOutput | Where-Object { $_ -match 'Native static mask:' } |
        Select-Object -First 1
    if ($signature -notmatch '0xC1FFE, dynamic objects: 20') {
        throw @"
El carrier MAT esta activo, pero el renderer no es Dolby Atmos ($signature).
Selecciona Dolby Atmos en Sonido espacial; no basta con cambiar solamente Formato.
"@
    }
}

Write-Host "$Mode spatial provider ready."
Write-Host $currentFormat
$spatialOutput | Where-Object { $_ -match 'Native static mask:' } | Select-Object -First 1
