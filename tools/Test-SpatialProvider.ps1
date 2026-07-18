[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Atmos', 'NativeMat', 'DtsX', 'Pcm')]
    [string]$Mode,

    [string]$EndpointFilter = '1 - HISENSE (Virtual Audio Device'
)

$repoRoot = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repoRoot 'build\SpatialAudioLab.CLI.exe'
if (-not (Test-Path $exe)) {
    throw "SpatialAudioLab.CLI.exe was not found: $exe"
}

$formatOutput = @(& $exe device-format $EndpointFilter 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) {
    throw "Could not inspect the spatial endpoint.`n$($formatOutput -join "`n")"
}
$currentFormat = $formatOutput | Where-Object { $_ -match '^\s*device current:' } |
    Select-Object -First 1
$mixFormat = $formatOutput | Where-Object { $_ -match '^\s*mix:' } |
    Select-Object -First 1
$formatMatches = if ($Mode -in @('Atmos', 'NativeMat')) {
    $currentFormat -match 'Dolby (MLP / MAT 1\.0|MAT 2\.)'
} elseif ($Mode -eq 'DtsX') {
    $currentFormat -match 'DTS:X E[12]'
} else {
    $currentFormat -match '12ch, 48000 Hz, 16 bit, block=24, mask=0x2D63F, PCM'
}
if (-not $formatMatches) {
    $wanted = if ($Mode -in @('Atmos', 'NativeMat')) {
        'Dolby Atmos para el centro de entretenimiento'
    } elseif ($Mode -eq 'DtsX') {
        'DTS:X para centro de entretenimiento'
    } else {
        'PCM nativo 7.1.4'
    }
    throw @"
El carrier activo no corresponde a $Mode ($currentFormat).
El selector automatico debe activar '$wanted' antes de iniciar el puente.
"@
}

if ($Mode -eq 'NativeMat') {
    if ($mixFormat -notmatch '8ch, 48000 Hz, 32 bit, block=32, mask=0x63F, IEEE_FLOAT') {
        throw "Native MAT requires the 7.1 shared mix format ($mixFormat)."
    }

    $endpointId = $formatOutput | Where-Object {
        $_ -match '^ID:\s*\{0\.0\.0\.00000000\}\.\{(?<Endpoint>[0-9a-f-]{36})\}'
    } | Select-Object -First 1
    if ($endpointId -notmatch '^ID:\s*\{0\.0\.0\.00000000\}\.\{(?<Endpoint>[0-9a-f-]{36})\}') {
        throw 'Could not resolve the endpoint registry key for native MAT.'
    }

    $registryPath = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\' +
        "{$($Matches.Endpoint)}\Properties"
    $properties = Get-ItemProperty -LiteralPath $registryPath -ErrorAction Stop
    $enabled = $properties.'{6737016f-5360-48ee-af05-e616c8ff27a7},2'
    if ($null -eq $enabled -or $enabled.Length -lt 9 -or $enabled[8] -ne 0) {
        throw 'The Dolby MAT carrier is selected, but Windows spatial audio is still enabled.'
    }

    Write-Host 'Native MAT carrier ready with Windows spatial audio disabled.'
    Write-Host $mixFormat
    Write-Host $currentFormat
    return
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
} elseif ($Mode -eq 'DtsX') {
    $signature -match '0xFFFFE, dynamic objects: 32'
} else {
    $signature -match 'dynamic objects: 0'
}
if (-not $signatureMatches) {
    throw "El carrier esta activo, pero la firma espacial no corresponde a $Mode ($signature)."
}

Write-Host "$Mode spatial provider ready."
Write-Host $currentFormat
Write-Host $signature
