[CmdletBinding()]
param(
    [ValidateSet('Driver', 'Cavern', 'All')]
    [string[]]$Component = @('All')
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$requested = @($Component | ForEach-Object { $_.ToLowerInvariant() })
if ($requested -contains 'all') {
    $requested = @('driver', 'cavern')
}

$nativeGit = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $nativeGit) {
    $nativeGit = Get-Command git -ErrorAction SilentlyContinue
}
$wsl = if (-not $nativeGit) { Get-Command wsl.exe -ErrorAction SilentlyContinue } else { $null }
if (-not $nativeGit -and -not $wsl) {
    throw 'Git was not found. Install Git for Windows or enable WSL before initializing dependencies.'
}

function Invoke-GitCommand([string]$Repository, [string[]]$Arguments) {
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($nativeGit) {
            $output = @(& $nativeGit.Source -C $Repository @Arguments 2>&1)
        } else {
            $portableRepository = $Repository.Replace('\', '/')
            $wslRepository = (& $wsl.Source wslpath -a $portableRepository 2>$null).Trim()
            if (-not $wslRepository) {
                throw "WSL could not translate the repository path: $Repository"
            }
            $output = @(& $wsl.Source git -C $wslRepository @Arguments 2>&1)
        }
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    [pscustomobject]@{
        ExitCode = $exitCode
        Output = ($output -join [Environment]::NewLine)
    }
}

function Get-RelativePath([string]$BasePath, [string]$TargetPath) {
    $base = (Resolve-Path $BasePath).Path.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $target = (Resolve-Path $TargetPath).Path
    $baseUri = [Uri]::new($base)
    $targetUri = [Uri]::new($target)
    [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString())
}

function Install-Patch([string]$Name, [string]$PatchPath, [string]$DestinationPath) {
    if (-not (Test-Path (Join-Path $DestinationPath '.git'))) {
        throw "$Name submodule is missing. Clone with --recurse-submodules or run git submodule update --init --recursive."
    }

    $relativePatch = Get-RelativePath $DestinationPath $PatchPath
    $checkArguments = @(
        'apply', '--check', '--ignore-space-change', '--ignore-whitespace', $relativePatch
    )
    $forward = Invoke-GitCommand $DestinationPath $checkArguments
    if ($forward.ExitCode -eq 0) {
        $applyArguments = @(
            'apply', '--whitespace=nowarn', '--ignore-space-change', '--ignore-whitespace',
            $relativePatch
        )
        $applied = Invoke-GitCommand $DestinationPath $applyArguments
        if ($applied.ExitCode -ne 0) {
            throw "$Name patch failed:`n$($applied.Output)"
        }
        Write-Host "$Name patch applied."
        return
    }

    $reverseArguments = @(
        'apply', '--reverse', '--check', '--ignore-space-change', '--ignore-whitespace',
        $relativePatch
    )
    $reverse = Invoke-GitCommand $DestinationPath $reverseArguments
    if ($reverse.ExitCode -eq 0) {
        Write-Host "$Name patch already applied."
        return
    }
    throw "$Name patch does not match the submodule state:`n$($forward.Output)"
}

foreach ($name in @($requested | Select-Object -Unique)) {
    switch ($name) {
        'driver' {
            Install-Patch 'SpatialAudioLab Virtual Sink' `
                (Join-Path $repoRoot 'patches\windows-driver-samples\SpatialAudioLab.patch') `
                (Join-Path $repoRoot 'driver\windows-driver-samples')
        }
        'cavern' {
            Install-Patch 'Cavern' `
                (Join-Path $repoRoot 'patches\Cavern\SpatialAudioLab.patch') `
                (Join-Path $repoRoot 'third_party\Cavern')
        }
    }
}
