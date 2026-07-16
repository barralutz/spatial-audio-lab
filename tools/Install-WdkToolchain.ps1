[CmdletBinding()]
param()

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator. The installers require elevation.'
}

$winget = Get-Command winget.exe -ErrorAction Stop
$config = Join-Path $PSScriptRoot 'wdk.vsconfig'
$vsInstaller = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$requiredVsComponents = @(
    'Component.Microsoft.Windows.DriverKit'
    'Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre'
)

function Test-WingetPackageInstalled {
    param([Parameter(Mandatory)][string]$Id)

    & $winget.Source list --source winget --exact --id $Id `
        --accept-source-agreements *> $null
    return $LASTEXITCODE -eq 0
}

if (Test-WingetPackageInstalled 'Microsoft.VisualStudio.2022.Community') {
    Write-Host 'Visual Studio 2022 Community is already installed.'
} else {
    Write-Host 'Installing Visual Studio 2022 Community with the C++ driver prerequisites...'
    & $winget.Source install --source winget --exact `
        --id Microsoft.VisualStudio.2022.Community `
        --accept-source-agreements --accept-package-agreements `
        --override "--wait --passive --config `"$config`""
    if ($LASTEXITCODE -ne 0) { throw "Visual Studio installation failed: $LASTEXITCODE" }
}

if (-not (Test-Path $vsInstaller) -or -not (Test-Path $vswhere)) {
    throw 'Visual Studio Installer was not found after installing Visual Studio.'
}

$vsInstallPath = & $vswhere -latest -products Microsoft.VisualStudio.Product.Community `
    -property installationPath
if (-not $vsInstallPath) {
    throw 'Visual Studio 2022 Community installation path was not found.'
}

$missingVsComponents = @($requiredVsComponents | Where-Object {
    -not (& $vswhere -latest -products Microsoft.VisualStudio.Product.Community `
        -requires $_ -property installationPath)
})
if ($missingVsComponents.Count -eq 0) {
    Write-Host 'Visual Studio driver build components are already installed.'
} else {
    Write-Host "Adding Visual Studio components: $($missingVsComponents -join ', ')"
    $componentArguments = @($missingVsComponents | ForEach-Object { '--add'; $_ })
    & $vsInstaller modify --installPath $vsInstallPath `
        @componentArguments --passive --norestart
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio component installation failed: $LASTEXITCODE"
    }
}

if (Test-WingetPackageInstalled 'Microsoft.WindowsSDK.10.0.26100') {
    Write-Host 'Windows SDK 26100 is already installed.'
} else {
    Write-Host 'Installing the Windows 11 SDK (build family 26100)...'
    & $winget.Source install --source winget --exact `
        --id Microsoft.WindowsSDK.10.0.26100 `
        --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) { throw "Windows SDK installation failed: $LASTEXITCODE" }
}

if (Test-WingetPackageInstalled 'Microsoft.WindowsWDK.10.0.26100') {
    Write-Host 'WDK 26100 is already installed.'
} else {
    Write-Host 'Installing WDK 10.0.26100.6584...'
    & $winget.Source install --source winget --exact `
        --id Microsoft.WindowsWDK.10.0.26100 `
        --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) { throw "WDK installation failed: $LASTEXITCODE" }
}

Write-Host 'Toolchain installation completed. Open a new terminal before building.'
