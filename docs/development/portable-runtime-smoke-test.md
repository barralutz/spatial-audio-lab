# Portable runtime smoke test

This procedure verifies that SpatialAudioLab Studio, Cinema and the native Engine run from a clean
directory outside the repository. It intentionally uses framework-dependent development builds;
release packaging will publish self-contained applications from the same runtime layout.

## Runtime layout

```text
SpatialAudioLab-portable-smoke/
|-- SpatialAudioLab.Studio.exe
|-- SpatialAudioLab.Cinema.exe
|-- portable.flag
|-- Data/
|-- Engine/
|   `-- SpatialAudioLab.CLI.exe
`-- Tools/
    |-- ffmpeg/bin/ffprobe.exe
    |-- mpv/mpv.exe
    `-- truehdd/truehd-stream.exe
```

`portable.flag` makes every application use `Data` in this directory. Without it, user data lives
under `%LocalAppData%\SpatialAudioLab`. Neither mode may read or write the repository's `configs`
or `captures` directories.

## Build and stage

Run these commands from the repository root in PowerShell:

```powershell
$ErrorActionPreference = 'Stop'
$Stage = Join-Path $env:TEMP 'SpatialAudioLab-portable-smoke'

dotnet build .\tools\SpeakerLayoutEditor\SpeakerLayoutEditor.csproj -c Release
dotnet build .\tools\DolbyPlayer\DolbyPlayer.csproj -c Release
.\tools\Build-DolbyProbe.ps1

Remove-Item $Stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item $Stage -ItemType Directory | Out-Null
New-Item "$Stage\Engine" -ItemType Directory | Out-Null
New-Item "$Stage\Tools\ffmpeg\bin" -ItemType Directory | Out-Null
New-Item "$Stage\Tools\mpv" -ItemType Directory | Out-Null
New-Item "$Stage\Tools\truehdd" -ItemType Directory | Out-Null

Copy-Item '.\tools\SpeakerLayoutEditor\bin\Release\net8.0-windows\*' $Stage -Recurse -Force
Copy-Item '.\tools\DolbyPlayer\bin\Release\net8.0-windows\*' $Stage -Recurse -Force
Copy-Item '.\build\SpatialAudioLab.CLI.exe' "$Stage\Engine\SpatialAudioLab.CLI.exe"
Copy-Item (Get-Command ffprobe.exe).Source "$Stage\Tools\ffmpeg\bin\ffprobe.exe"
Copy-Item 'C:\Program Files\MPV Player\mpv.exe' "$Stage\Tools\mpv\mpv.exe"
Copy-Item '.\tools\truehdd\truehd-stream.exe' "$Stage\Tools\truehdd\truehd-stream.exe"
New-Item "$Stage\portable.flag" -ItemType File | Out-Null
```

The copy commands create only temporary files below `$env:TEMP`; no generated executable belongs
in Git.

## Verify isolation

Capture the repository state before launching either application:

```powershell
function Get-RepositoryRuntimeState {
    Get-ChildItem '.\configs', '.\captures' -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName |
        ForEach-Object {
            '{0}|{1}|{2}' -f $_.FullName, $_.Length, $_.LastWriteTimeUtc.Ticks
        }
}

$Before = @(Get-RepositoryRuntimeState)
Start-Process "$Stage\SpatialAudioLab.Studio.exe"
```

On a clean stage, Studio must show that initial speaker configuration is required and display
`$Stage\Data\Profiles` instead of loading `configs\realtek-c1u-714.ini`. Close Studio, then verify
that `Data\Profiles`, `Data\Logs` and `Data\Cache` exist below `$Stage`.

Choose an E-AC-3 JOC or TrueHD Atmos Matroska file and run the Cinema probe directly:

```powershell
$Media = 'D:\Media\Atmos Movie.mkv'
& "$Stage\SpatialAudioLab.Cinema.exe" inspect $Media
if ($LASTEXITCODE -notin 0, 2) { throw "Cinema inspect failed: $LASTEXITCODE" }

$After = @(Get-RepositoryRuntimeState)
if (Compare-Object $Before $After) {
    throw 'Portable runtime modified repository configs or captures.'
}
```

Exit code `0` means an Atmos stream was found; `2` means probing succeeded but the file contains no
supported Atmos stream. Any other exit code is a smoke-test failure.

Remove the stage when finished:

```powershell
Remove-Item $Stage -Recurse -Force
```
