[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$InputFile,

    [ValidateRange(0, 86400)]
    [double]$StartSeconds = 600,

    [ValidateRange(1, 3600)]
    [double]$DurationSeconds = 60,

    [ValidateRange(0, 255)]
    [int]$AudioStreamIndex = 1,

    [string]$OutputBase = '',

    [switch]$KeepIntermediate
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$truehdd = Join-Path $PSScriptRoot 'truehdd\truehdd.exe'
$cavern = Join-Path $PSScriptRoot 'Cavern712\bin\Release\net8.0\Cavern712.dll'

if (-not (Test-Path $truehdd)) {
    throw "truehdd is not installed. Run .\tools\Install-Truehdd.ps1 first."
}
if (-not (Test-Path $cavern)) {
    throw "Cavern712 is not built. Run: dotnet build .\tools\Cavern712\Cavern712.csproj -c Release"
}
$inputPath = (Resolve-Path $InputFile).Path
if ([string]::IsNullOrWhiteSpace($OutputBase)) {
    $OutputBase = Join-Path $repoRoot 'captures\truehd-atmos-test'
} elseif (-not [IO.Path]::IsPathRooted($OutputBase)) {
    $OutputBase = Join-Path (Get-Location) $OutputBase
}
$OutputBase = [IO.Path]::GetFullPath($OutputBase)
$outputDirectory = Split-Path $OutputBase -Parent
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$trueHdPath = "$OutputBase.thd"
$damfBase = "$OutputBase-damf"
$atmosPath = "$damfBase.atmos"
$wavePath = "$OutputBase-712.wav"

function ConvertTo-WslPath([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    if ($fullPath -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "Only local Windows drive paths can be passed to WSL FFmpeg: $fullPath"
    }
    $drive = $Matches[1].ToLowerInvariant()
    $relative = $Matches[2].Replace('\', '/')
    return "/mnt/$drive/$relative"
}

function Invoke-FfmpegDemux {
    $start = $StartSeconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    $duration = $DurationSeconds.ToString([Globalization.CultureInfo]::InvariantCulture)
    $windowsFfmpeg = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
    if ($null -ne $windowsFfmpeg) {
        & $windowsFfmpeg.Source -hide_banner -loglevel fatal -y -ss $start -i $inputPath `
            -ss 0 -t $duration -map "0:$AudioStreamIndex" -vn -sn -dn `
            -c:a copy -f truehd $trueHdPath
    } else {
        if ($null -eq (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
            throw 'ffmpeg.exe was not found and WSL is unavailable.'
        }
        $wslInput = ConvertTo-WslPath $inputPath
        $wslOutput = ConvertTo-WslPath $trueHdPath
        & wsl.exe ffmpeg -hide_banner -loglevel fatal -y -ss $start -i $wslInput `
            -ss 0 -t $duration -map "0:$AudioStreamIndex" -vn -sn -dn `
            -c:a copy -f truehd $wslOutput
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $trueHdPath)) {
        throw "FFmpeg failed to extract TrueHD stream 0:$AudioStreamIndex."
    }
}

Remove-Item $trueHdPath, "$damfBase.atmos", "$damfBase.atmos.audio", `
    "$damfBase.atmos.metadata", $wavePath -Force -ErrorAction SilentlyContinue

Write-Host "Extracting $DurationSeconds seconds of TrueHD from stream 0:$AudioStreamIndex..."
Invoke-FfmpegDemux

Write-Host 'Inspecting TrueHD presentation...'
& $truehdd info $trueHdPath
if ($LASTEXITCODE -ne 0) {
    throw "truehdd info failed with exit code $LASTEXITCODE."
}

Write-Host 'Decoding Atmos presentation 3 to DAMF...'
& $truehdd decode --progress --presentation 3 $trueHdPath --output-path $damfBase
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $atmosPath)) {
    throw 'truehdd did not produce a presentation 3 DAMF file. The selected track may not contain Atmos objects.'
}

Write-Host 'Rendering DAMF objects to PCM16 7.1.2...'
& dotnet $cavern render $atmosPath $wavePath $DurationSeconds
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $wavePath)) {
    throw "Cavern712 render failed with exit code $LASTEXITCODE."
}

if (-not $KeepIntermediate) {
    Remove-Item $trueHdPath, "$damfBase.atmos", "$damfBase.atmos.audio", `
        "$damfBase.atmos.metadata" -Force -ErrorAction SilentlyContinue
}

Write-Host "Ready for analog playback: $wavePath"
Write-Host "Play with: .\build\dolby-probe.exe play-712 `"$wavePath`" `"Altavoces (Realtek(R) Audio)`" `"2nd output`" 0.5 1"
