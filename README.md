# SpatialAudioLab

Experimental Windows spatial-audio platform for capturing, decoding, rendering and routing
immersive audio to configurable multi-device speaker layouts.

SpatialAudioLab currently turns a virtual Windows spatial endpoint into an analog **7.1.4** output
distributed across several WASAPI devices. It supports live game audio through Dolby MAT, DTS:X or
native PCM, and real-time movie playback from E-AC-3 JOC and TrueHD Atmos sources.

> [!WARNING]
> This is research software, not a production audio driver or a finished installer. The current
> capture path uses a test-signed SysVAD driver, private Windows endpoint state observed on one
> Windows build, and licensed third-party spatial providers. A failed driver or endpoint experiment
> can temporarily remove audio until the driver or Windows audio services are restored.

## Current capabilities

| Area | Status |
| --- | --- |
| Dolby MAT game capture and render | Working live in 7.1.4 |
| DTS:X game capture and render | Working live in 7.1.4 |
| Native Windows Spatial PCM | Working live with a static 7.1.4 bed |
| E-AC-3 JOC movie playback | Working in real time through PCM 7.1.4 |
| TrueHD Atmos movie playback | Working in real time through PCM 7.1.4 |
| Multiple audio endpoints | Working with adaptive clock-drift correction |
| Graphical speaker placement and routing | Working in SpatialAudioLab Studio |
| Per-channel RMS and peak meters | Working for all live bridge modes |
| Production-signed virtual driver | Not available |
| General-purpose Windows spatial provider | Research stage |

The validated layout uses a Realtek 7.1 endpoint plus two stereo endpoints for four height
speakers. USB DACs, front-panel outputs and additional PCIe audio devices can be combined as long
as Windows exposes them as independent render endpoints.

## Components

| Component | Purpose | Artifact |
| --- | --- | --- |
| **SpatialAudioLab Studio** | Speaker placement, endpoint routing, bridge controls and meters | `SpatialAudioLab.Studio.exe` |
| **SpatialAudioLab Cinema** | E-AC-3 JOC and TrueHD Atmos movie playback | `SpatialAudioLab.Cinema.exe` |
| **SpatialAudioLab Router** | Live MAT, DTS:X and PCM routing modes | Hosted by the CLI |
| **SpatialAudioLab Virtual Sink** | Test SysVAD endpoint and kernel-to-user capture ring | Windows driver package |
| **SpatialAudioLab CLI** | Diagnostics, capture, analysis and live routing engine | `SpatialAudioLab.CLI.exe` |
| **SpatialAudioLab Engine** | Multi-endpoint render, resampling and clock correction | C++ core |

```mermaid
flowchart LR
    A[Game or Cinema decoder] --> B[SpatialAudioLab Virtual Sink]
    B --> C[MAT / DTS:X / PCM capture ring]
    C --> D[SpatialAudioLab Router]
    D --> E[SpatialAudioLab Engine]
    E --> F[Realtek 7.1]
    E --> G[USB DAC / front panel]
    E --> H[Additional DAC]
    I[SpatialAudioLab Studio] --> D
    I --> E
```

## Requirements

- Windows 11 x64. Development and validation currently target build `10.0.26200.8655`.
- Visual Studio 2022 with Desktop C++ and driver development prerequisites.
- Windows SDK and WDK from the 26100 build family.
- .NET 8 SDK for Studio and Cinema.
- Administrator access and a boot with driver signature enforcement disabled for the test driver.
- Dolby Access for the Dolby MAT route or a licensed DTS Sound Unbound installation for DTS:X.
- [mpv](https://mpv.io/) for video presentation in SpatialAudioLab Cinema.
- At least one 48 kHz analog render endpoint; multiple endpoints are required for layouts that
  exceed the channel count of a single device.

Do not run games protected by kernel anti-cheat while the test-signed driver is loaded. Return to a
normal signed-driver boot before using those titles.

## Build from source

Clone the repository with its submodules:

```powershell
git clone --recurse-submodules https://github.com/barralutz/spatial-audio-lab.git
cd spatial-audio-lab
```

Apply the versioned SpatialAudioLab overlays to SysVAD and Cavern, inspect and install the driver
toolchain, then build the CLI and Studio:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
./tools/Initialize-SpatialAudioLab.ps1
./tools/Install-WdkToolchain.ps1
./tools/Build-SpatialAudioLab.ps1
```

Cinema additionally needs the bundled TrueHD streaming decoder and its .NET dependencies:

```powershell
./tools/Install-Truehdd.ps1
./tools/Build-SpatialAudioLab.ps1 -Component Cinema
```

The virtual sink is a test driver. Read the complete one-boot procedure before installing it:

- [Virtual sink driver guide](driver/README.md)
- [Technical research notes](docs/TECHNICAL_NOTES.md)

## First run

Start the graphical layout and routing application:

```powershell
./tools/Start-SpatialAudioLabStudio.ps1
```

The included `configs/realtek-c1u-714.ini` describes the original development system and is an
example, not a portable hardware preset. Copy it, select your own render endpoints in the
**Outputs** tab, assign logical speakers to physical channels, test each speaker at low gain, and
save the resulting profile before starting a live bridge.

Studio can switch among three router modes:

- **Dolby MAT** for a Windows Dolby Atmos for Home Theater stream.
- **DTS:X** for a Windows DTS:X for Home Theater stream.
- **PCM 7.1.4** for a native static spatial bed and for Cinema playback.

All bridge modes run until explicitly stopped. The current latency profiles are Safe, Balanced and
Low; Balanced is the recommended starting point.

## Cinema

Install a desktop shortcut after building Cinema:

```powershell
./tools/Install-SpatialAudioLabCinemaShortcut.ps1
```

Open **SpatialAudioLab Cinema** from the desktop and select an `.mkv` or `.mka` file. The launcher
prepares the PCM 7.1.4 bridge, keeps mpv synchronized to the WASAPI render clock and supports both
E-AC-3 JOC and TrueHD Atmos inputs. Decode support does not imply that every title contains active
height objects; Studio's channel meters show what the renderer is producing.

## Project status

This repository records a working prototype and the investigation that produced it. Hardware names,
endpoint IDs and private provider state are currently machine- and Windows-build-specific. The next
major step is turning installation, endpoint discovery and profile creation into a reproducible setup
flow that does not assume the original development PC.

The detailed captures, format analysis, validation results and recovery procedures live in
[the technical notes](docs/TECHNICAL_NOTES.md). Contributions should preserve the separation between
the kernel capture path and user-mode codec/render logic.

## Third-party projects

SpatialAudioLab integrates or studies several projects as Git submodules, including Microsoft
SysVAD, Cavern, FFmpeg, `truehdd` and `dolby-atmos-encoder`. Each dependency retains its own license
and upstream history. The small source changes required by Virtual Sink and Cinema are kept as
[versioned patches](patches/README.md), so all submodule gitlinks resolve to public upstream
commits. Dolby, Dolby Atmos, DTS, DTS:X and Windows are trademarks of their respective owners;
SpatialAudioLab is an independent experimental project.
