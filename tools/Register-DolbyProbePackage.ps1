[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path $PSScriptRoot -Parent
$sourceManifest = Join-Path $repoRoot 'packaging\AppxManifest.xml'
$sourceExe = Join-Path $repoRoot 'build\SpatialAudioLab.CLI.exe'
$packageRoot = Join-Path $repoRoot 'build-package\SpatialAudioLabCLI'
$assetsRoot = Join-Path $packageRoot 'Assets'

if (-not (Test-Path $sourceExe)) {
    throw 'build\SpatialAudioLab.CLI.exe was not found. Run Build-DolbyProbe.ps1 first.'
}

New-Item -ItemType Directory -Path $packageRoot, $assetsRoot -Force | Out-Null
Copy-Item $sourceManifest (Join-Path $packageRoot 'AppxManifest.xml') -Force
Copy-Item $sourceExe (Join-Path $packageRoot 'SpatialAudioLab.CLI.exe') -Force

Add-Type -AssemblyName System.Drawing
function New-PackageLogo([string]$Path, [int]$Size) {
    $bitmap = [System.Drawing.Bitmap]::new($Size, $Size)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::FromArgb(32, 37, 43))
        $pen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(82, 196, 126),
                                         [Math]::Max(2, [int]($Size / 12)))
        try {
            $margin = [int]($Size / 4)
            $graphics.DrawEllipse($pen, $margin, $margin, $Size - 2 * $margin,
                                  $Size - 2 * $margin)
        } finally {
            $pen.Dispose()
        }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

New-PackageLogo (Join-Path $assetsRoot 'StoreLogo.png') 50
New-PackageLogo (Join-Path $assetsRoot 'Square44x44Logo.png') 44
New-PackageLogo (Join-Path $assetsRoot 'Square150x150Logo.png') 150

$existing = Get-AppxPackage -Name 'barra.DolbyDecoderProbe'
if ($existing) {
    Remove-AppxPackage -Package $existing.PackageFullName
}

$developmentPolicy = Get-ItemProperty `
    -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' `
    -Name AllowDevelopmentWithoutDevLicense `
    -ErrorAction SilentlyContinue
if ($developmentPolicy.AllowDevelopmentWithoutDevLicense -eq 1) {
    Add-AppxPackage -Register (Join-Path $packageRoot 'AppxManifest.xml')
} else {
    $sdkBin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64' } |
        Where-Object { Test-Path (Join-Path $_ 'makeappx.exe') } |
        Select-Object -First 1
    if (-not $sdkBin) {
        throw 'makeappx.exe was not found in the Windows SDK.'
    }

    $certificate = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object {
            $_.Subject -eq 'CN=barra' -and
            $_.FriendlyName -in @('SpatialAudioLab Development',
                                   'Dolby Decoder Probe Development')
        } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1
    if (-not $certificate -or $certificate.NotAfter -le (Get-Date).AddDays(30)) {
        $certificate = New-SelfSignedCertificate `
            -Type Custom `
            -Subject 'CN=barra' `
            -FriendlyName 'SpatialAudioLab Development' `
            -CertStoreLocation 'Cert:\CurrentUser\My' `
            -KeyAlgorithm RSA `
            -KeyLength 2048 `
            -HashAlgorithm SHA256 `
            -KeyUsage DigitalSignature `
            -KeyExportPolicy NonExportable `
            -NotAfter (Get-Date).AddYears(2) `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
    }

    $certificatePath = Join-Path $repoRoot 'build-package\SpatialAudioLab.cer'
    Export-Certificate -Cert $certificate -FilePath $certificatePath -Force | Out-Null
    $trusted = Get-ChildItem Cert:\CurrentUser\TrustedPeople |
        Where-Object Thumbprint -eq $certificate.Thumbprint
    if (-not $trusted) {
        Import-Certificate -FilePath $certificatePath `
            -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople' | Out-Null
    }
    $machineTrusted = Get-ChildItem Cert:\LocalMachine\TrustedPeople |
        Where-Object Thumbprint -eq $certificate.Thumbprint
    if (-not $machineTrusted) {
        Write-Host 'Windows will request administrator approval to trust the local package certificate.'
        $certutil = Join-Path $env:SystemRoot 'System32\certutil.exe'
        $trustProcess = Start-Process $certutil -Verb RunAs -Wait -PassThru `
            -ArgumentList @('-f', '-addstore', 'TrustedPeople', $certificatePath)
        if ($trustProcess.ExitCode -ne 0) {
            throw "Installing the package certificate in LocalMachine\TrustedPeople failed: $($trustProcess.ExitCode)"
        }
    }

    $msixPath = Join-Path $repoRoot 'build-package\SpatialAudioLab.CLI.msix'
    & (Join-Path $sdkBin 'makeappx.exe') pack /d $packageRoot /p $msixPath /o
    if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed: $LASTEXITCODE" }
    & (Join-Path $sdkBin 'signtool.exe') sign /fd SHA256 /s My `
        /sha1 $certificate.Thumbprint $msixPath
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed: $LASTEXITCODE" }
    Add-AppxPackage -Path $msixPath
}

$registered = Get-AppxPackage -Name 'barra.DolbyDecoderProbe'
if (-not $registered) {
    throw 'The development package did not register.'
}
Write-Host "Registered: $($registered.PackageFullName)"
Write-Host 'Console alias: spatial-audio-lab-cli.exe'
