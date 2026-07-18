# Release Packaging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a reproducible Windows x64 staging directory and produce an installer plus portable
ZIP that contain every redistributable runtime dependency, no development-machine data, and enough
license/version information to audit every file.

**Architecture:** A committed dependency lock controls external downloads and hashes. One PowerShell
publisher builds/stages applications, driver, tools, presets, licenses, and manifest; validators
gate the stage before both ZIP and Inno Setup consume it.

**Tech Stack:** PowerShell 7/Windows PowerShell 5.1-compatible scripts, Pester 5, .NET 8 publish,
CMake/MSVC, WDK 26100 family, Inno Setup 6, JSON manifests, SHA-256.

## Global Constraints

- This plan starts only after runtime, onboarding, and Setup lifecycle plans are green.
- The only user artifacts are `SpatialAudioLab-Setup-<version>-x64.exe` and
  `SpatialAudioLab-Portable-<version>-x64.zip`.
- Both artifacts consume the same validated staging directory.
- Dolby Access and DTS Sound Unbound are never bundled.
- Captures, personal profiles, absolute development paths, logs, symbols, source build directories,
  and unsigned/unmanifested binaries are rejected.
- External dependency URLs, versions, and SHA-256 hashes are committed before publication.
- Production behavior is written only after its failing automated test has been observed.

---

### Task 1: Project and Third-Party Licensing

**Files:**
- Create: `LICENSE`
- Create: `packaging/THIRD_PARTY_COMPONENTS.json`
- Create: `tools/packaging/New-ThirdPartyNotices.ps1`
- Create: `tests/package/ThirdPartyNotices.Tests.ps1`
- Modify: `README.md`

**Interfaces:**
- Root original code is MIT.
- Produces staged `Licenses/*` and `THIRD_PARTY_NOTICES.txt` from a machine-readable inventory.

- [ ] **Step 1: Write failing Pester inventory tests**

Require entries for Windows Driver Samples, Cavern, truehdd, FFmpeg, mpv, and Inno Setup with
`Name`, `VersionSource`, `License`, `LicenseFile`, `Homepage`, `Distribution`, and
`IncludedArtifacts`. Reject a missing license file and an inventory path outside repository root.

- [ ] **Step 2: Run and verify RED**

```powershell
Invoke-Pester .\tests\package\ThirdPartyNotices.Tests.ps1 -Output Detailed
```

Expected: failure because root license/inventory/generator are absent.

- [ ] **Step 3: Add exact MIT license and scoped README wording**

Use the canonical MIT text with copyright `2026 SpatialAudioLab contributors`. State that it covers
original SpatialAudioLab code only; submodules and derived patches retain their own terms. Do not
claim the Cavern-combined Cinema binary is MIT-only.

- [ ] **Step 4: Implement deterministic notices generation**

Sort components by `Name`, copy each declared license into `Licenses/<component>/`, and generate a
plain-text notice listing component, version source, license, homepage, included artifacts, and any
distribution restriction. Cavern's non-commercial/public-use terms must appear verbatim by license
file reference and in a short warning summary.

- [ ] **Step 5: Run tests and commit**

```powershell
Invoke-Pester .\tests\package\ThirdPartyNotices.Tests.ps1 -Output Detailed
git add LICENSE README.md packaging/THIRD_PARTY_COMPONENTS.json tools/packaging/New-ThirdPartyNotices.ps1 tests/package
git commit -m "License SpatialAudioLab original code under MIT"
```

---

### Task 2: Pinned Runtime Dependency Lock

**Files:**
- Create: `packaging/dependencies.lock.json`
- Create: `tools/packaging/DependencyLock.psm1`
- Create: `tools/packaging/Update-DependencyLock.ps1`
- Create: `tools/packaging/Get-LockedDependencies.ps1`
- Create: `tests/package/DependencyLock.Tests.ps1`
- Modify: `.gitignore`

**Interfaces:**
- Lock fields: `name`, `version`, `url`, `sha256`, `archiveType`, `licensePath`, and `files`.
- Initial downloaded dependencies: a standard x86_64 Shinchiro mpv build and a BtbN win64 LGPL
  FFmpeg build containing `ffprobe.exe`. `truehd-stream.exe` is built from the pinned truehdd
  submodule instead of downloaded.

- [ ] **Step 1: Write failing lock/fetch tests**

Test lowercase 64-character SHA-256, HTTPS URL, unique names, explicit file allowlist, archive-slip
rejection, cache hit, checksum mismatch, missing declared output, and no executable copied outside
the dependency staging directory.

- [ ] **Step 2: Run and verify RED**

```powershell
Invoke-Pester .\tests\package\DependencyLock.Tests.ps1 -Output Detailed
```

- [ ] **Step 3: Implement lock update behavior**

`Update-DependencyLock.ps1` queries upstream GitHub release APIs only when explicitly invoked,
selects non-`v3` x86_64 mpv and win64 LGPL FFmpeg assets, downloads them, computes SHA-256, and
writes a fully pinned lock. The ordinary publisher never follows `latest` and never changes the
lock.

- [ ] **Step 4: Implement locked fetch/extraction**

Cache archives below `build-dependencies/cache`, verify before extraction, support ZIP and 7z
through a pinned/existing extractor, and copy only the declared files. `files` maps archive-relative
paths to staged paths under `Tools/mpv` or `Tools/ffmpeg/bin`.

- [ ] **Step 5: Generate and review the committed lock**

```powershell
.\tools\packaging\Update-DependencyLock.ps1
Invoke-Pester .\tests\package\DependencyLock.Tests.ps1 -Output Detailed
```

Review every URL and hash before commit. Add `/build-dependencies/`, `/stage/`, and `/artifacts/` to
`.gitignore`.

- [ ] **Step 6: Commit**

```bash
git add .gitignore packaging/dependencies.lock.json tools/packaging tests/package
git commit -m "Pin portable runtime dependencies"
```

---

### Task 3: Self-Contained Application and Engine Publication

**Files:**
- Create: `tools/packaging/Publish-Applications.ps1`
- Create: `tests/package/PublishedApplications.Tests.ps1`
- Modify: `tools/Build-SpatialAudioLab.ps1`
- Modify: `tools/SpatialAudioLab.Setup/SpatialAudioLab.Setup.csproj`
- Modify: `tools/SpeakerLayoutEditor/SpeakerLayoutEditor.csproj`
- Modify: `tools/DolbyPlayer/DolbyPlayer.csproj`

**Interfaces:**
- Produces an application stage containing self-contained Setup, Studio, Cinema, native Engine,
  DriverHelper, and `truehd-stream.exe` built from the pinned source submodule.

- [ ] **Step 1: Write failing publication-content tests**

Require all five executables, `.runtimeconfig.json` where appropriate, no `.pdb`, no `obj/bin`, no
repository-root dependency, and successful `--version` or `--help` process exit from the stage.

- [ ] **Step 2: Run and verify RED**

```powershell
Invoke-Pester .\tests\package\PublishedApplications.Tests.ps1 -Output Detailed
```

- [ ] **Step 3: Implement .NET self-contained publishing**

Run `dotnet publish -c Release -r win-x64 --self-contained true` for Setup, Studio, and Cinema into
isolated temporary directories. Set `PublishSingleFile=false` so native/runtime files remain
auditable and serviceable. Copy each application into stage root according to the approved layout.

- [ ] **Step 4: Publish native executables and the TrueHD streaming decoder**

Configure/build CMake Release x64, then copy `SpatialAudioLab.CLI.exe` to `Engine` and
`SpatialAudioLab.DriverHelper.exe` to `Driver`. Fail if either binary is older than its newest source
input. Verify `third_party/truehdd-src` is at commit `cb0849f46619ed8d10ae8f4ca0972fff7f519024`,
then run:

```powershell
cargo build --manifest-path .\third_party\truehdd-src\Cargo.toml `
  --profile release-deploy --bin truehd-stream
```

Copy the resulting `truehd-stream.exe` to `Tools\truehdd` and its Apache-2.0 license to the license
stage. Rust is a release-build dependency only and is not installed on user systems.

- [ ] **Step 5: Run tests and commit**

```powershell
.\tools\packaging\Publish-Applications.ps1 -StageRoot .\stage\apps
Invoke-Pester .\tests\package\PublishedApplications.Tests.ps1 -Output Detailed
git add tools/packaging/Publish-Applications.ps1 tools/Build-SpatialAudioLab.ps1 tools/SpatialAudioLab.Setup/SpatialAudioLab.Setup.csproj tools/SpeakerLayoutEditor/SpeakerLayoutEditor.csproj tools/DolbyPlayer/DolbyPlayer.csproj tests/package
git commit -m "Publish self-contained SpatialAudioLab applications"
```

---

### Task 4: Driver Package Staging

**Files:**
- Create: `tools/packaging/Publish-DriverPackage.ps1`
- Create: `tests/package/PublishedDriver.Tests.ps1`
- Modify: `tools/Install-SysvadCapture.ps1`

**Interfaces:**
- Produces `Driver/package`, certificate, driver manifest, and DriverHelper in staging.

- [ ] **Step 1: Write failing driver-stage tests**

Require CAT, INF, SYS, certificate, owned hardware ID, expected driver version, manifest thumbprint,
and matching SHA-256. Reject build logs, PDBs, APO samples not required by the Virtual Sink, and any
INF containing the old SysVAD hardware ID.

- [ ] **Step 2: Run and verify RED**

```powershell
Invoke-Pester .\tests\package\PublishedDriver.Tests.ps1 -Output Detailed
```

- [ ] **Step 3: Build and stage the minimal Release driver**

Apply/verify the SysVAD overlay, invoke the WDK Release x64 targets, select only the base component
package needed by the Virtual Sink, copy the generated certificate, and write `driver-manifest.json`
with package version, hardware ID, certificate thumbprint, and hashes.

- [ ] **Step 4: Run tests and commit**

```powershell
.\tools\packaging\Publish-DriverPackage.ps1 -StageRoot .\stage\driver
Invoke-Pester .\tests\package\PublishedDriver.Tests.ps1 -Output Detailed
git add tools/packaging/Publish-DriverPackage.ps1 tools/Install-SysvadCapture.ps1 tests/package
git commit -m "Stage the portable virtual sink driver"
```

---

### Task 5: Unified Stage and Release Manifest

**Files:**
- Create: `tools/Publish-SpatialAudioLab.ps1`
- Create: `tools/packaging/Test-ReleaseStage.ps1`
- Create: `tests/package/ReleaseStage.Tests.ps1`
- Create: `packaging/presets-manifest.json`

**Interfaces:**
- Main command: `Publish-SpatialAudioLab.ps1 -Version <SemVer> [-SkipInstaller]`.
- Produces validated `stage/SpatialAudioLab` and `release-manifest.json`.

- [ ] **Step 1: Write failing release-stage tests**

Require exact top-level layout, preset coverage, every file represented by relative path/size/hash,
no duplicate case-insensitive paths, SemVer product version, and absence of forbidden content:
`barra`, `seba`, drive-letter absolute paths, `/mnt/`, `realtek-c1u-714`, `captures`, `.pdb`, `.obj`,
`.pml`, `.etl`, `.wav`, `.log`, and private media extensions outside test fixtures.

- [ ] **Step 2: Run and verify RED**

```powershell
Invoke-Pester .\tests\package\ReleaseStage.Tests.ps1 -Output Detailed
```

- [ ] **Step 3: Implement the orchestrator**

Validate SemVer, ensure required submodule overlays match, clean only the dedicated stage directory,
publish apps/driver/dependencies, export every Core preset to `Presets`, generate notices, then write
a sorted JSON manifest with product version, Git commit, Windows target, dependency versions, and
per-file SHA-256.

- [ ] **Step 4: Implement the final stage gate**

`Test-ReleaseStage.ps1` independently recomputes hashes, scans forbidden paths/content, checks
licenses, launches `--help` smoke commands, and fails with one diagnostic per violation.

- [ ] **Step 5: Run full stage and commit**

```powershell
.\tools\Publish-SpatialAudioLab.ps1 -Version 0.1.0 -SkipInstaller
.\tools\packaging\Test-ReleaseStage.ps1 -StageRoot .\stage\SpatialAudioLab
Invoke-Pester .\tests\package -Output Detailed
git add tools/Publish-SpatialAudioLab.ps1 tools/packaging/Test-ReleaseStage.ps1 packaging/presets-manifest.json tests/package
git commit -m "Build a validated portable release stage"
```

---

### Task 6: Portable ZIP

**Files:**
- Create: `tools/packaging/New-PortableArchive.ps1`
- Create: `tests/package/PortableArchive.Tests.ps1`

**Interfaces:**
- Produces `artifacts/SpatialAudioLab-Portable-<version>-x64.zip`.

- [ ] **Step 1: Write failing archive tests**

Require `portable.flag`, no precreated `Data` contents, exact manifest/hash equivalence after
extraction, top-level `SpatialAudioLab` directory, and successful Setup/Studio executable discovery.

- [ ] **Step 2: Run and verify RED**

```powershell
Invoke-Pester .\tests\package\PortableArchive.Tests.ps1 -Output Detailed
```

- [ ] **Step 3: Implement deterministic archive creation**

Copy validated stage to a temporary `SpatialAudioLab` root, add zero-byte `portable.flag`, normalize
timestamps to `release-manifest.json` build time, create ZIP, re-extract it, and run the stage
validator against extracted contents.

- [ ] **Step 4: Run tests and commit**

```powershell
.\tools\packaging\New-PortableArchive.ps1 -Version 0.1.0
Invoke-Pester .\tests\package\PortableArchive.Tests.ps1 -Output Detailed
git add tools/packaging/New-PortableArchive.ps1 tests/package
git commit -m "Package SpatialAudioLab portable ZIP"
```

---

### Task 7: Inno Setup Installer

**Files:**
- Create: `packaging/SpatialAudioLab.iss`
- Create: `tools/packaging/New-Installer.ps1`
- Create: `tests/package/InstallerDefinition.Tests.ps1`

**Interfaces:**
- Produces `artifacts/SpatialAudioLab-Setup-<version>-x64.exe` from the validated stage.

- [ ] **Step 1: Write failing installer-definition tests**

Require x64-only install, `%ProgramFiles%\SpatialAudioLab`, stable AppId, Add/Remove Programs entry,
Studio/Cinema/Setup shortcuts, upgrade preserving LocalAppData, Setup launch after install, Setup
uninstall-driver invocation before file removal, and no direct driver installation in Inno code.

- [ ] **Step 2: Run and verify RED**

```powershell
Invoke-Pester .\tests\package\InstallerDefinition.Tests.ps1 -Output Detailed
```

- [ ] **Step 3: Implement the Inno definition**

Use per-machine administrative install, LZMA2 compression, signed-uninstaller disabled for this
unsigned preview, stable AppId `{C21600C7-BA68-4E7F-89E0-77B730517140}`, semantic DisplayVersion,
and shortcuts for Studio, Cinema, Setup, repair, and uninstall. Launch Setup unelevated after files
are installed.

- [ ] **Step 4: Implement compiler discovery and post-build checks**

Find `ISCC.exe` through explicit parameter, registry, then standard Inno Setup 6 path. Compile with
version/stage/artifact defines. Verify output filename, PE x64 bootstrap behavior, version metadata,
and SHA-256 before returning success.

- [ ] **Step 5: Build/test and commit**

```powershell
.\tools\packaging\New-Installer.ps1 -Version 0.1.0
Invoke-Pester .\tests\package\InstallerDefinition.Tests.ps1 -Output Detailed
git add packaging/SpatialAudioLab.iss tools/packaging/New-Installer.ps1 tests/package
git commit -m "Build the SpatialAudioLab installer"
```

---

### Task 8: End-to-End Publication and Clean-Machine Validation

**Files:**
- Create: `docs/release/0.1.0-validation.md`
- Create: `docs/release/RELEASING.md`
- Modify: `README.md`

- [ ] **Step 1: Run the complete publisher**

```powershell
.\tools\Publish-SpatialAudioLab.ps1 -Version 0.1.0
Invoke-Pester .\tests\package -Output Detailed
dotnet test .\SpatialAudioLab.sln -c Release
ctest --test-dir build-tests -C Release --output-on-failure
```

Expected: both artifacts exist and every automated test passes.

- [ ] **Step 2: Validate installer on a clean Windows 11 VM**

Verify normal-boot install, guided advanced startup, resumed driver install, 5.1.2 onboarding,
PCM bridge, profile persistence, later normal reboot detection, repair, upgrade over the same
version, and profile-preserving uninstall.

- [ ] **Step 3: Validate portable ZIP on a second clean directory/VM snapshot**

Extract, run Setup, install driver, create a 7.1.4 or capacity-appropriate profile, verify data is
created only below `Data`, inspect/play a supported movie, then remove driver without registering
the application in Add/Remove Programs.

- [ ] **Step 4: Validate hardware/provider matrix**

Record results for Windows stable and 10.0.26200.8655, single multichannel output, Realtek plus USB,
two USB devices, PCM, MAT, native MAT, DTS:X, E-AC-3 JOC, TrueHD Atmos, hot unplug, suspend/resume,
and endpoint switching. Mark unsupported combinations as explicit release notes, not silent passes.

- [ ] **Step 5: Record hashes/docs and commit**

```bash
git add README.md docs/release
git commit -m "Document SpatialAudioLab 0.1.0 release validation"
```
