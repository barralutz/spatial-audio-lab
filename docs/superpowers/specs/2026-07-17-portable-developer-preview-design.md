# SpatialAudioLab Portable Developer Preview Design

**Date:** 2026-07-17  
**Status:** Approved for implementation planning  
**Target release:** 0.1.0 Developer Preview

## Objective

Produce an installer and a portable ZIP that let a technically competent Windows user run
SpatialAudioLab without cloning the repository or installing Visual Studio, the WDK, Rust, or the
.NET SDK. The user accepts the temporary Windows boot option **Disable driver signature
enforcement** because production driver signing is outside this release.

The release must discover the user's actual audio hardware, support standard physical speaker
layouts from 2.0 through 7.1.4, install the existing virtual sink, and expose the existing PCM,
Dolby MAT, DTS:X, E-AC-3 JOC, and TrueHD Atmos paths through graphical applications.

## Scope

### Included

- Windows 11 x64 installer built with Inno Setup.
- Windows 11 x64 portable ZIP built from the same staged files.
- Self-contained SpatialAudioLab Setup, Studio, and Cinema applications.
- Native SpatialAudioLab CLI/Engine and the x64 SysVAD test-driver package.
- Redistributable runtime dependencies, licenses, version manifest, and SHA-256 hashes.
- Guided installation and resumption after the user chooses **Disable driver signature
  enforcement** in Windows advanced startup.
- First-run speaker-layout selection, endpoint discovery, channel assignment by test tone, profile
  creation, and profile management.
- Standard layouts from 2.0 through 7.1.4, including 5.1.2, 5.1.4, 7.1.2, and 7.1.4.
- Front, middle, or rear placement variants for a single height pair.
- Custom layouts containing 2 through 12 logical speakers within the 7.1.4 physical limit.
- Installed and portable configuration-storage modes.
- Installation repair, driver repair, uninstall, rollback, and privacy-conscious diagnostics.

### Excluded

- Production driver signing and Windows Hardware Developer Center submission.
- Enabling `TESTSIGNING`, disabling Secure Boot, or changing BitLocker configuration.
- Automatic acoustic measurement with a microphone.
- Output layouts above 7.1.4, including 9.1.6.
- An automatic application updater.
- Bundling Dolby Access or DTS Sound Unbound.
- Claiming compatibility with every Windows build, spatial provider, protected game, or audio
  driver.

## Deliverables

The release pipeline produces these two user-facing artifacts:

```text
SpatialAudioLab-Setup-0.1.0-x64.exe
SpatialAudioLab-Portable-0.1.0-x64.zip
```

Both artifacts originate from one staged directory:

```text
SpatialAudioLab/
|-- SpatialAudioLab.Studio.exe
|-- SpatialAudioLab.Cinema.exe
|-- SpatialAudioLab.Setup.exe
|-- Engine/
|   `-- SpatialAudioLab.CLI.exe
|-- Driver/
|   `-- x64 SysVAD driver package
|-- Tools/
|   |-- mpv/
|   `-- redistributable decoders
|-- Presets/
|   `-- standard speaker layouts
|-- Licenses/
|-- THIRD_PARTY_NOTICES.txt
`-- release-manifest.json
```

Inno Setup owns installed-file deployment, Start menu and desktop shortcuts, Add or Remove
Programs registration, upgrades, and uninstall. SpatialAudioLab Setup owns system preflight,
driver installation, advanced-startup guidance, repair, and diagnostics.

The portable ZIP contains `portable.flag`. It does not register the application with Windows, but
installing or updating its virtual audio driver still requires elevation.

## Runtime Paths

Installed mode uses:

```text
%ProgramFiles%\SpatialAudioLab\             read-only application files
%LocalAppData%\SpatialAudioLab\             user profiles, preferences, and application logs
%ProgramData%\SpatialAudioLab\Setup\        elevated setup state and setup logs
```

Portable mode uses:

```text
<application directory>\Data\               profiles, preferences, and application logs
```

All applications resolve their components from the installed or portable application root. They
must not search for `CMakeLists.txt`, a repository root, a `build` directory, or the original
development paths.

## Component Boundaries

### SpatialAudioLab Setup

- Runs unelevated for inspection and requests elevation only for system changes.
- Reports application, driver, Virtual Sink, PCM, Atmos, DTS:X, and Cinema readiness separately.
- Installs, updates, repairs, and removes the SpatialAudioLab-owned SysVAD device and certificate.
- Persists resumable setup state before requesting advanced startup.
- Verifies success by discovering the endpoint and opening the kernel capture control device.
- Creates a diagnostic archive only after showing the user what it contains.

### SpatialAudioLab Studio

- Owns first-run onboarding and profile management.
- Enumerates render endpoints and their stable IDs, names, channel counts, and supported exclusive
  48 kHz formats.
- Proposes physical routing, plays channel-isolation tones, and records confirmed assignments.
- Starts, stops, and monitors PCM, MAT, native MAT, and DTS:X Router modes.
- Retains the existing advanced position, trim, output delay, and meter interfaces after onboarding.

### SpatialAudioLab Cinema

- Resolves Engine, mpv, Cavern, and TrueHD components from the application root.
- Opens media through the existing graphical file picker and reports the selected immersive audio
  path.
- Uses the active Studio profile and the PCM Router rather than a repository configuration.

### SpatialAudioLab CLI/Engine

- Remains the native audio engine and diagnostic command surface.
- Accepts an explicit profile path from Studio, Cinema, or Setup.
- Resolves endpoints using persistent identity first and validated fallback metadata second.
- Performs layout-aware rendering, downmixing, multi-device routing, clock correction, and bridge
  metering.

### SpatialAudioLab Virtual Sink

- Continues to expose the validated 12-channel PCM and IEC 61937/MAT/DTS formats.
- Retains the observed Hisense manufacturer, product, port, and sink-description compatibility
  properties for release 0.1.0.
- Exposes `SpatialAudioLab Virtual Sink` as the product-facing endpoint name wherever Windows
  permits a separate friendly name.
- Treats the Hisense properties as an explicit compatibility implementation, not a claim that the
  user owns or is connected to that hardware.

## First-Run Experience

Studio starts onboarding only when no valid active profile exists.

### 1. System readiness

The page reports:

- Virtual Sink installation and load state.
- Active render endpoints.
- Dolby Access installation and Atmos availability.
- DTS Sound Unbound installation, license, and DTS:X availability.
- Cinema dependencies.
- Whether this boot permits the test driver to load.

Missing Dolby or DTS applications open their Microsoft Store pages only after an explicit user
action. They are never installed silently.

### 2. Layout selection

The preset catalog includes standard non-height arrangements from 2.0 through 7.1 and the spatial
arrangements 5.1.2, 5.1.4, 7.1.2, and 7.1.4. A two-height layout offers front, middle, and rear
placement variants. Four-height layouts use front and rear pairs.

A custom option lets the user select any valid subset of the canonical 7.1.4 speaker catalog. A
custom layout contains 2 through 12 logical speakers, no more than seven ear-level speakers, no more
than one LFE, and no more than four height speakers. This supports arrangements such as 3.1.2,
4.1.2, 4.1.4, 6.1, and asymmetric research layouts without claiming support above 7.1.4.

Presets initialize canonical azimuth and elevation, `0 dB` trim, and `0 ms` delay. Automatic
microphone calibration and layouts above 7.1.4 are unavailable.

### 3. Endpoint discovery and proposal

Studio probes each active render endpoint without changing the default Windows device. It records:

- MMDevice endpoint ID.
- Friendly name.
- Device or container identity when available.
- Reported channel count.
- Exact exclusive PCM support at 48 kHz.
- The physical channels that can be isolated by the test renderer.

The proposal favors one multichannel endpoint as the master and adds stereo endpoints only when
needed. It never assumes Realtek, C-1U, USB, PCIe, front-panel, or rear-panel naming.

### 4. Channel assignment

The user triggers a low-level tone on one physical channel and chooses the logical speaker that was
heard. Studio prevents duplicate logical assignments, reports missing speakers, and allows unused
physical channels. The user can go backward and replace any endpoint or assignment.

### 5. Verification and save

Studio plays a sequential low-level test over all assigned speakers, shows the resulting routing,
and saves the profile only after validation succeeds. The profile becomes active and the normal
Studio interface opens.

## Profile Format and Resolution

Profiles remain human-readable INI documents so the native Engine and .NET applications can share
one format. Version 2 adds:

- Schema version and stable profile UUID.
- User-facing profile name and logical layout identifier.
- Persistent endpoint ID.
- Friendly-name fallback.
- Expected channel count and format capability.
- Logical-to-physical channel assignments.
- Speaker position and trim.
- Route and speaker delay fields.
- Master-clock route.

Studio supports create, duplicate, rename, import, export, activate, and delete. The original
`configs/realtek-c1u-714.ini` remains an example and migration source, not a default.

Endpoint resolution follows this order:

1. Exact persistent endpoint ID.
2. A unique match on stored device/container identity and compatible capabilities.
3. A unique match on friendly name and compatible capabilities.
4. User confirmation in Studio.

Ambiguous matches never reassign speakers automatically. Profile writes use a temporary file and
atomic replacement.

## Driver and Advanced-Startup Flow

The product uses the Windows label **Disable driver signature enforcement**. The term `F7` is not a
product concept, although Setup may mention the currently displayed keyboard shortcut as secondary
guidance.

From a normal boot, Inno Setup installs the application files. SpatialAudioLab Setup then:

1. Inspects whether the current boot can load the test driver.
2. Stages a resumable operation without installing a driver that cannot load.
3. Offers **Restart into advanced startup**.
4. Registers a one-time continuation for the next interactive logon.
5. Opens Windows advanced startup after confirmation.
6. Resumes after the user selects **Disable driver signature enforcement**.
7. Installs or updates the driver, discovers the Virtual Sink, and opens the capture device.
8. Launches Studio onboarding.

An advanced user may start the installer after already selecting the Windows boot option; Setup
then completes without another restart.

After a later normal reboot, Studio reports that the Virtual Sink driver is unavailable and offers
the same guided restart. It does not alter Secure Boot, BitLocker, or the persistent boot policy.

## Spatial Provider Switching

Every mode transition is transactional:

```text
Stop Router
-> save current endpoint/provider state
-> select Virtual Sink
-> configure PCM, MAT, or DTS:X format
-> select and validate the requested provider
-> start Router
-> roll back saved state if any required step fails
```

Public Windows APIs are attempted first. Existing private MMDevices property manipulation is moved
behind an experimental repair operation. Repair requires elevation, creates a backup, checks the
Windows build, verifies the result, and restores the backup on failure.

Hard-coded PnP instance IDs such as `ROOT\MEDIA\0001` are prohibited. Setup discovers its device by
the SpatialAudioLab hardware ID and validates ownership before update or removal.

## Failure Handling and Recovery

- Setup stores its state before each system-changing operation and can resume or roll back.
- Driver success requires both PnP readiness and an openable capture control device.
- Router startup failure restores the prior Windows endpoint and spatial-provider state.
- A disconnected physical endpoint marks only its routes unavailable and opens reassignment in
  Studio; it does not silently route those speakers elsewhere.
- Corrupt or unsupported profiles are preserved, reported, and excluded from activation.
- Uninstall removes only the SpatialAudioLab-owned root device, driver package, and certificate.
- Uninstall asks separately whether to remove profiles and logs.

## Diagnostics and Privacy

Normal application logs contain component versions, HRESULT/Win32 status, endpoint capabilities,
profile IDs, buffer statistics, and state transitions. They omit media paths, Windows account names,
and unrelated devices unless the user explicitly enables detailed diagnostics.

Diagnostic export lists every included file and value before creating the archive. The user can
cancel or remove individual optional items.

## Publication Pipeline

The release command is:

```powershell
./tools/Publish-SpatialAudioLab.ps1 -Version 0.1.0
```

It performs these operations in order:

1. Validate clean parent sources and pinned submodule commits.
2. Apply or verify the versioned SysVAD and Cavern overlays.
3. Build Release x64 CLI/Engine and the SysVAD package.
4. Publish Setup, Studio, and Cinema as self-contained `win-x64` applications.
5. Stage pinned redistributable dependencies.
6. Generate `THIRD_PARTY_NOTICES.txt`, copy licenses, and write a version manifest with SHA-256
   hashes.
7. Reject captures, personal profiles, absolute development paths, build logs, and unlicensed
   dependencies.
8. Create the portable ZIP.
9. Compile the Inno Setup installer from the same staged directory.
10. Run package-content and launch smoke tests.

The first release has no automatic updater. Inno Setup supports an in-place upgrade while
preserving user data. Portable upgrades replace application files but leave `Data` untouched.

## Licensing

Original SpatialAudioLab code is released under the MIT License. The root license explicitly
excludes submodules and code derived from third-party projects, which retain their upstream terms.

- `truehdd` retains Apache-2.0.
- Microsoft Windows Driver Samples retain their upstream license.
- Cavern and Cavern-derived changes retain the Cavern custom license, including its non-commercial
  restrictions.
- FFmpeg/mpv distribution follows the license of the exact selected build and includes the
  corresponding notices and source-offer obligations.
- Dolby Access and DTS Sound Unbound are user-installed licensed products and are not distributed.

Publication fails when a staged binary lacks a pinned version, checksum, license file, or required
notice.

## Testing Strategy

Implementation follows test-driven development. Pure profile, preset, path, endpoint-resolution,
setup-state, and package-validation behavior is moved behind testable libraries instead of WPF
window code.

Automated coverage includes:

- Every supported preset and height-pair variant.
- Invalid, incomplete, duplicate, and over-capacity routing.
- Profile version 2 serialization, atomic replacement, and version 1 import.
- Exact and fallback endpoint resolution, including ambiguous matches.
- Installed and portable application-root/data-root resolution.
- Resumable Setup state transitions and rollback decisions.
- Router start, stop, provider rollback, and disconnected-route state.
- Release manifest, hashes, licenses, forbidden paths, and artifact contents.

Hardware validation before release includes:

- Windows 11 stable and Windows build 10.0.26200.8655.
- One multichannel endpoint.
- Realtek plus one USB DAC.
- Two independent USB endpoints.
- PCM, Dolby MAT, legacy native MAT, DTS:X, E-AC-3 JOC, and TrueHD Atmos.
- Endpoint switching, suspend/resume, hot unplug, and a later normal reboot.
- New install, advanced-startup continuation, upgrade, repair, and uninstall.

## Acceptance Criteria

On a Windows 11 x64 computer that has no repository or development toolchain, a user can:

1. Install the EXE or extract the ZIP.
2. Follow the guided **Disable driver signature enforcement** restart when required.
3. Install and verify the Virtual Sink without entering commands.
4. Select a standard preset or create a custom layout within the 2.0 to 7.1.4 limit.
5. Assign the detected physical channels by listening to test tones.
6. Save and reactivate the profile after reopening Studio.
7. Start an available PCM, MAT, native MAT, or DTS:X Router mode from Studio.
8. Play supported E-AC-3 JOC or TrueHD Atmos media from Cinema.
9. Receive a precise, recoverable error when a provider, license, driver, or endpoint is missing.
10. Repair or uninstall the product without manually editing the registry or Driver Store.
