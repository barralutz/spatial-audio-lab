[CmdletBinding()]
param(
    [switch]$KeepCertificate
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator. Driver removal requires elevation.'
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $repoRoot 'driver\windows-driver-samples\audio\sysvad\Package\x64'
$certificatePaths = @(Get-ChildItem $buildRoot -Filter 'package.cer' -Recurse `
    -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName -Unique)
$devcon = Join-Path ${env:ProgramFiles(x86)} `
    'Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe'
$hardwareId = 'Root\sysvad_ComponentizedAudioSample'

if (Test-Path $devcon) {
    & $devcon remove $hardwareId
    if ($LASTEXITCODE -notin 0, 1) {
        Write-Warning "DevCon could not remove the root device: $LASTEXITCODE"
    }
}

$originalInfNames = @(
    'ComponentizedAudioSampleExtension.inf'
    'ComponentizedApoSample.inf'
    'ComponentizedAudioSample.inf'
)
$sysvadDrivers = @(Get-WindowsDriver -Online | Where-Object {
    $_.OriginalFileName -and
    (Split-Path $_.OriginalFileName -Leaf) -in $originalInfNames
})
foreach ($driver in $sysvadDrivers) {
    Write-Host "Removing driver package $($driver.Driver)..."
    & pnputil.exe /delete-driver $driver.Driver /uninstall /force
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "PnPUtil could not remove $($driver.Driver): $LASTEXITCODE"
    }
}

if (-not $KeepCertificate) {
    foreach ($certificatePath in $certificatePaths) {
        $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
        foreach ($storeName in 'TrustedPublisher', 'Root') {
            $storePath = "Cert:\LocalMachine\$storeName\$($certificate.Thumbprint)"
            if (Test-Path $storePath) {
                Remove-Item $storePath -Force
                Write-Host "Removed the test certificate from LocalMachine\$storeName."
            }
        }
    }
}

Write-Host 'SysVAD capture driver removal complete.'
