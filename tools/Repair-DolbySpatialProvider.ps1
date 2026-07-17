[CmdletBinding()]
param()

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator.'
}

$repoRoot = Split-Path $PSScriptRoot -Parent
& (Join-Path $PSScriptRoot 'Stop-Live714.ps1')
& (Join-Path $PSScriptRoot 'Stop-LiveDtsX714.ps1')

$package = Get-AppxPackage -Name 'DolbyLaboratories.DolbyAccess'
if ($null -eq $package) {
    throw 'Dolby Access is not installed for the current user.'
}

Get-Process DolbyAccess -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Add-AppxPackage -DisableDevelopmentMode -Register `
    (Join-Path $package.InstallLocation 'AppxManifest.xml')

Stop-Service Audiosrv -Force
Restart-Service AudioEndpointBuilder -Force
Start-Service Audiosrv
Start-Sleep -Seconds 3

Start-Process 'shell:AppsFolder\DolbyLaboratories.DolbyAccess_rz1tebttyb220!App'
Start-Process 'ms-settings:sound'
Write-Host 'Dolby Access and its codecs were re-registered without deleting app data.'
Write-Host 'In Dolby Access, configure Dolby Atmos for Home Theater.'
Write-Host 'Then select Dolby Atmos under Spatial sound for SinkDescription Sample.'
