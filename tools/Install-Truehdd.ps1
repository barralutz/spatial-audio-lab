[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$version = '0.4.0'
$archiveName = "truehdd-$version-x86_64-pc-windows-msvc.zip"
$archiveUri = "https://github.com/truehdd/truehdd/releases/download/$version/$archiveName"
$expectedSha256 = '50a9ae1f6284cc7468c824f68053bd9e31363559bb7d56a718d3aaf1c587d0bc'
$licenseUri = 'https://raw.githubusercontent.com/truehdd/truehdd/main/LICENSE'
$installRoot = Join-Path $PSScriptRoot 'truehdd'
$executable = Join-Path $installRoot 'truehdd.exe'

if (Test-Path $executable) {
    $installedVersion = & $executable --version
    if ($LASTEXITCODE -eq 0 -and $installedVersion -match [regex]::Escape($version)) {
        Write-Host "truehdd $version is already installed: $executable"
        exit 0
    }
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) "dolbyDecoder-truehdd-$version"
$archivePath = Join-Path $tempRoot $archiveName
Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

try {
    Write-Host "Downloading truehdd $version..."
    Invoke-WebRequest -UseBasicParsing -Uri $archiveUri -OutFile $archivePath
    $actualSha256 = (Get-FileHash -Algorithm SHA256 $archivePath).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $expectedSha256) {
        throw "SHA-256 mismatch for $archiveName. Expected $expectedSha256, got $actualSha256."
    }

    Remove-Item $installRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
    Expand-Archive -Path $archivePath -DestinationPath $installRoot -Force
    Invoke-WebRequest -UseBasicParsing -Uri $licenseUri -OutFile (Join-Path $installRoot 'LICENSE')

    if (-not (Test-Path $executable)) {
        throw "The archive did not contain truehdd.exe at the expected path: $executable"
    }
    & $executable --version
    if ($LASTEXITCODE -ne 0) {
        throw "truehdd.exe failed its version check with exit code $LASTEXITCODE."
    }
    Set-Content -Path (Join-Path $installRoot 'VERSION') -Value $version -Encoding Ascii
    Write-Host "Installed: $executable"
} finally {
    Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
