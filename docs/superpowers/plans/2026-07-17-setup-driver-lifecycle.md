# Setup and Driver Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide a graphical, resumable, and recoverable way to install the test-signed Virtual
Sink, guide Windows advanced startup, validate spatial providers, repair failures, and uninstall
only SpatialAudioLab-owned system state.

**Architecture:** A self-contained WPF Setup application drives a persisted state machine and calls
a narrowly scoped native driver helper. System inspection is unelevated; individual operations
request elevation. Provider switching becomes a transaction with public API first and an isolated,
backed-up experimental registry repair.

**Tech Stack:** C# 12/.NET 8 WPF, xUnit, C++20 SetupAPI/NewDev, Windows PnPUtil, WinRT spatial audio.

## Global Constraints

- The user accepts temporary **Disable driver signature enforcement** boots.
- Setup never enables `TESTSIGNING`, disables Secure Boot, or changes BitLocker.
- No hard-coded PnP instance ID is permitted.
- Driver operations target only hardware ID `Root\SpatialAudioLabVirtualSink`.
- Setup persists state before every system-changing operation.
- Studio and Cinema remain unelevated.
- Production behavior is written only after its failing automated test has been observed.

---

### Task 1: Resumable Setup State Machine

**Files:**
- Create: `tools/SpatialAudioLab.Core/Setup/SetupPhase.cs`
- Create: `tools/SpatialAudioLab.Core/Setup/SetupState.cs`
- Create: `tools/SpatialAudioLab.Core/Setup/SetupStateStore.cs`
- Create: `tools/SpatialAudioLab.Core/Setup/SetupCoordinator.cs`
- Create: `tests/SpatialAudioLab.Core.Tests/SetupCoordinatorTests.cs`

**Interfaces:**
- Produces phases `Inspect`, `AwaitingAdvancedRestart`, `InstallDriver`, `VerifyDriver`,
  `ConfigureProviders`, `Complete`, `Repair`, `Uninstall`, and `Failed`.
- Produces atomic `%ProgramData%\SpatialAudioLab\Setup\state.json` persistence.

- [ ] **Step 1: Write failing transition and resume tests**

Test normal-boot deferral, already-ready direct install, resume token mismatch, failed verification,
retry, rollback, completion, and uninstall. Assert state is saved before an operation callback runs:

```csharp
await coordinator.AdvanceAsync();
Assert.Equal(
    [SetupPhase.AwaitingAdvancedRestart, SetupPhase.InstallDriver],
    store.SavedStates.Select(state => state.Phase));
```

- [ ] **Step 2: Run and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj `
  --filter FullyQualifiedName~SetupCoordinatorTests
```

- [ ] **Step 3: Implement state and atomic store**

`SetupState` contains schema version, operation UUID, phase, application root, portable flag,
driver package version, certificate thumbprint, installed INF name, previous endpoint ID, previous
provider GUID, last completed action, error code, and error message. Reject state whose application
root no longer exists or whose resume token differs.

- [ ] **Step 4: Run tests and commit**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj
git add tools/SpatialAudioLab.Core/Setup tests/SpatialAudioLab.Core.Tests
git commit -m "Add resumable setup state machine"
```

---

### Task 2: System and Driver Preflight

**Files:**
- Create: `tools/SpatialAudioLab.Core/Setup/SystemReadiness.cs`
- Create: `tools/SpatialAudioLab.Core/Setup/ISystemReadinessProbe.cs`
- Create: `tools/SpatialAudioLab.Core/Setup/ICodeIntegrityProbe.cs`
- Create: `tools/SpatialAudioLab.Setup/Services/WindowsSystemReadinessProbe.cs`
- Create: `tools/SpatialAudioLab.Setup/Services/WindowsCodeIntegrityProbe.cs`
- Create: `tests/SpatialAudioLab.Core.Tests/SystemReadinessProbeTests.cs`

**Interfaces:**
- Produces separate status for OS, administrator capability, code integrity, PnP device, capture
  device, Virtual Sink endpoint, Dolby Access, DTS Sound Unbound, and Cinema tools.

- [ ] **Step 1: Write failing readiness aggregation tests**

Test ready driver, missing driver in normal boot, missing driver with enforcement disabled, loaded
device with missing capture control, missing provider applications, and absent Cinema tool. A
missing optional provider must not mark PCM unavailable.

- [ ] **Step 2: Run and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj `
  --filter FullyQualifiedName~SystemReadiness
```

- [ ] **Step 3: Keep contracts portable and implement Windows probes in Setup**

Keep readiness records and probe interfaces in SpatialAudioLab Core without Windows references.
Implement the concrete probes in SpatialAudioLab Setup: use `RtlGetVersion` for the real OS build,
`NtQuerySystemInformation(SystemCodeIntegrityInformation)` for code-integrity flags, Configuration
Manager/SetupAPI for the hardware ID, `CreateFile` on `\\.\DolbyDecoderMat` for capture readiness,
MMDevice enumeration for the endpoint, AppX package enumeration for Dolby/DTS, and `RuntimePaths`
for Cinema tools. Report `Unknown` instead of guessing when an API is unavailable.

- [ ] **Step 4: Run tests and commit**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj
git add tools/SpatialAudioLab.Core/Setup tools/SpatialAudioLab.Setup/Services tests/SpatialAudioLab.Core.Tests
git commit -m "Probe SpatialAudioLab system readiness"
```

---

### Task 3: Native Driver Helper Without DevCon

**Files:**
- Create: `driver/helper/CMakeLists.txt`
- Create: `driver/helper/main.cpp`
- Create: `driver/helper/driver_device.cpp`
- Create: `driver/helper/driver_device.h`
- Create: `driver/helper/json_output.cpp`
- Create: `driver/helper/json_output.h`
- Create: `tests/native/driver_helper_argument_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces `SpatialAudioLab.DriverHelper.exe inspect|install|restart|remove`.
- Every command emits one JSON object and uses hardware ID `Root\SpatialAudioLabVirtualSink`.

- [ ] **Step 1: Write failing command/ownership tests**

Test strict verb parsing, missing package, non-SpatialAudioLab hardware ID rejection, JSON escaping,
and exit-code mapping. Hardware-changing SetupAPI calls are behind an interface and use a fake in
unit tests.

- [ ] **Step 2: Build tests and verify RED**

```powershell
cmake --build build-tests --config Release --target SpatialAudioLab.NativeTests
ctest --test-dir build-tests -C Release --output-on-failure
```

- [ ] **Step 3: Implement root-device creation and update**

Create the root devnode with `SetupDiCreateDeviceInfo`, set `SPDRP_HARDWAREID` to the double-NUL
terminated SpatialAudioLab ID, register with `DIF_REGISTERDEVICE`, and install/update the supplied
INF with `UpdateDriverForPlugAndPlayDevicesW`. Enumerate and remove only matching devnodes. Return
instance ID, reboot-required flag, INF path/name, and Win32 code in JSON.

- [ ] **Step 4: Implement inspect/restart/remove**

Enumerate matching devices, reject more than one active owned root device as inconsistent state,
restart with `CM_Query_And_Remove_SubTree`/property change as appropriate, and remove through
SetupAPI. Do not delete an INF or certificate; Setup owns those separate operations.

- [ ] **Step 5: Build, inspect, and commit**

```powershell
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

Expected: tests pass; `SpatialAudioLab.DriverHelper.exe inspect` returns valid JSON without
elevation.

```bash
git add CMakeLists.txt driver/helper tests/native
git commit -m "Add owned virtual driver lifecycle helper"
```

---

### Task 4: SpatialAudioLab Setup Application Shell

**Files:**
- Create: `tools/SpatialAudioLab.Setup/SpatialAudioLab.Setup.csproj`
- Create: `tools/SpatialAudioLab.Setup/App.xaml`
- Create: `tools/SpatialAudioLab.Setup/App.xaml.cs`
- Create: `tools/SpatialAudioLab.Setup/MainWindow.xaml`
- Create: `tools/SpatialAudioLab.Setup/MainWindow.xaml.cs`
- Create: `tools/SpatialAudioLab.Setup/SetupViewModel.cs`
- Create: `tests/SpatialAudioLab.Setup.Tests/SpatialAudioLab.Setup.Tests.csproj`
- Create: `tests/SpatialAudioLab.Setup.Tests/SetupViewModelTests.cs`
- Modify: `SpatialAudioLab.sln`

**Interfaces:**
- Consumes `RuntimePaths`, `ISystemReadinessProbe`, and `SetupCoordinator`.
- Produces install, guided restart, resume, repair, diagnostics, and uninstall commands.

- [ ] **Step 1: Write failing view-model tests**

Assert status rows remain independent, only system-changing commands request elevation, guided
restart is offered only when appropriate, optional provider absence does not disable PCM, and error
details include action/code without exposing a stack trace by default.

- [ ] **Step 2: Run and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Setup.Tests\SpatialAudioLab.Setup.Tests.csproj
```

- [ ] **Step 3: Implement compact setup UI**

Use a status table with icons for Application, Driver, Virtual Sink, PCM, Atmos, DTS:X, and Cinema.
Commands appear in one bottom action bar. Do not use a marketing landing page or nested cards.

- [ ] **Step 4: Run tests/build and commit**

```powershell
dotnet test .\tests\SpatialAudioLab.Setup.Tests\SpatialAudioLab.Setup.Tests.csproj
dotnet build .\tools\SpatialAudioLab.Setup\SpatialAudioLab.Setup.csproj -c Release
git add tools/SpatialAudioLab.Setup tests/SpatialAudioLab.Setup.Tests SpatialAudioLab.sln
git commit -m "Add graphical SpatialAudioLab Setup"
```

---

### Task 5: Elevated Driver Installation Transaction

**Files:**
- Create: `tools/SpatialAudioLab.Setup/Services/ElevatedProcessRunner.cs`
- Create: `tools/SpatialAudioLab.Setup/Services/DriverInstaller.cs`
- Create: `tools/SpatialAudioLab.Setup/Services/CertificateInstaller.cs`
- Create: `tests/SpatialAudioLab.Setup.Tests/DriverInstallerTests.cs`
- Modify: `tools/Install-SysvadCapture.ps1`
- Modify: `tools/Uninstall-SysvadCapture.ps1`

**Interfaces:**
- Produces `DriverInstaller.InstallAsync`, `RepairAsync`, and `UninstallAsync`.
- Development PowerShell scripts become wrappers around packaged helper behavior.

- [ ] **Step 1: Write failing transaction tests**

With fake process/certificate/PnP services, test certificate import, helper install, PnP verification,
capture open, rollback after verification failure, reboot-required result, and uninstall preserving
unrelated certificates.

- [ ] **Step 2: Run and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Setup.Tests\SpatialAudioLab.Setup.Tests.csproj `
  --filter FullyQualifiedName~DriverInstallerTests
```

- [ ] **Step 3: Implement install and verification**

Import the packaged certificate into LocalMachine Root and TrustedPublisher only if its thumbprint
matches `release-manifest.json`. Invoke DriverHelper with the packaged INF, parse JSON, wait for the
owned PnP device, discover Virtual Sink, and open the capture control device. Record installed OEM
INF name and certificate thumbprint in setup state.

- [ ] **Step 4: Implement scoped uninstall**

Remove the owned root device through DriverHelper, invoke `pnputil /delete-driver <recorded-oem.inf>
/uninstall` only for the recorded package, and remove only the recorded certificate thumbprint when
no other SpatialAudioLab package references it.

- [ ] **Step 5: Run tests and commit**

```powershell
dotnet test .\tests\SpatialAudioLab.Setup.Tests\SpatialAudioLab.Setup.Tests.csproj
git add tools/SpatialAudioLab.Setup/Services tools/Install-SysvadCapture.ps1 tools/Uninstall-SysvadCapture.ps1 tests
git commit -m "Install and remove the portable virtual sink"
```

---

### Task 6: Guided Advanced-Startup Continuation

**Files:**
- Create: `tools/SpatialAudioLab.Setup/Services/AdvancedStartupService.cs`
- Create: `tools/SpatialAudioLab.Setup/Services/ResumeRegistration.cs`
- Create: `tests/SpatialAudioLab.Setup.Tests/AdvancedStartupTests.cs`

**Interfaces:**
- Produces `PrepareResume(Guid operationId)` and `RestartIntoAdvancedStartupAsync()`.

- [ ] **Step 1: Write failing continuation tests**

Assert exact RunOnce command quoting for spaces, operation-token propagation, state persistence
before registration, cleanup after successful resume, stale-token rejection, and cancellation before
`shutdown.exe` launch.

- [ ] **Step 2: Run and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Setup.Tests\SpatialAudioLab.Setup.Tests.csproj `
  --filter FullyQualifiedName~AdvancedStartup
```

- [ ] **Step 3: Implement one-time resume**

Write an HKLM RunOnce value that invokes the exact Setup executable with `--resume <operation-id>`.
After explicit confirmation launch `shutdown.exe /r /o /t 0`. On resume, request elevation only when
entering `InstallDriver`; delete RunOnce after token validation.

- [ ] **Step 4: Add exact product wording**

Primary UI text is **Disable driver signature enforcement**. Mention the numbered/key shortcut only
as secondary dynamic guidance. Never call the feature F7 internally.

- [ ] **Step 5: Run tests and commit**

```powershell
dotnet test .\tests\SpatialAudioLab.Setup.Tests\SpatialAudioLab.Setup.Tests.csproj
git add tools/SpatialAudioLab.Setup/Services tests/SpatialAudioLab.Setup.Tests
git commit -m "Resume setup after advanced startup"
```

---

### Task 7: Driver Product Identity and Package Ownership

**Files:**
- Modify: `driver/windows-driver-samples/audio/sysvad/TabletAudioSample/hdmitopo.cpp`
- Modify: relevant SysVAD INF files under `driver/windows-driver-samples/audio/sysvad`
- Modify: `patches/windows-driver-samples/SpatialAudioLab.patch`
- Create: `tests/package/driver-package.Tests.ps1`

**Interfaces:**
- Hardware ID is `Root\SpatialAudioLabVirtualSink`.
- Product-facing name is `SpatialAudioLab Virtual Sink`.
- Hisense compatibility manufacturer/product/port and sink descriptor remain unchanged internally.

- [ ] **Step 1: Write failing package assertions**

The Pester test inspects built/staged INF and source patch, requiring the owned hardware ID and
friendly name while rejecting `Root\sysvad_ComponentizedAudioSample` and the original sample
manufacturer text.

- [ ] **Step 2: Run and verify RED**

```powershell
Invoke-Pester .\tests\package\driver-package.Tests.ps1 -Output Detailed
```

- [ ] **Step 3: Change INF ownership and friendly identity**

Use new root hardware ID and product strings. Preserve `KSJACK_SINK_INFORMATION` compatibility
values used by Atmos/DTS:X. Rebuild the SysVAD overlay patch from exactly the modified source files
and verify reverse apply with the initializer's whitespace options.

- [ ] **Step 4: Build driver/package tests and commit**

```powershell
.\tools\Build-SpatialAudioLab.ps1 -Component CLI,Studio
Invoke-Pester .\tests\package\driver-package.Tests.ps1 -Output Detailed
```

```bash
git add patches/windows-driver-samples/SpatialAudioLab.patch tests/package
git commit -m "Give the virtual sink a portable product identity"
```

---

### Task 8: Transactional Spatial Provider Switching

**Files:**
- Create: `tools/SpatialAudioLab.Core/Spatial/SpatialMode.cs`
- Create: `tools/SpatialAudioLab.Core/Spatial/SpatialState.cs`
- Create: `tools/SpatialAudioLab.Core/Spatial/SpatialModeTransaction.cs`
- Create: `tools/SpatialAudioLab.Setup/Services/WindowsSpatialProvider.cs`
- Create: `tests/SpatialAudioLab.Core.Tests/SpatialModeTransactionTests.cs`
- Modify: `tools/SpeakerLayoutEditor/MainWindow.xaml.cs`
- Modify: `tools/Set-SpatialProvider.ps1`

**Interfaces:**
- Produces `ApplyAsync(SpatialMode mode)` and rollback to captured endpoint/provider state.

- [ ] **Step 1: Write failing transaction tests**

Test stop bridge, capture prior state, set default sink, set device format, set provider, preflight,
start bridge, and reverse-order rollback for a failure at every step. Assert rollback failure is
reported separately without hiding the original failure.

- [ ] **Step 2: Run and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj `
  --filter FullyQualifiedName~SpatialModeTransactionTests
```

- [ ] **Step 3: Implement public-API-first provider service**

Use MMDevice policy service already exercised by CLI for endpoint/device format and WinRT
`SpatialAudioDeviceConfiguration.SetDefaultSpatialAudioFormatAsync` for provider selection. Keep
provider GUIDs in named constants. Validate with the existing CLI preflight before bridge start.

- [ ] **Step 4: Integrate Studio**

Studio delegates mode changes to the transaction, disables conflicting controls during transition,
and shows the failed action plus rollback result. Stop uses the same transaction boundary so modes
cannot leave stale bridges.

- [ ] **Step 5: Run tests/build and commit**

```powershell
dotnet test .\SpatialAudioLab.sln
dotnet build .\tools\SpeakerLayoutEditor\SpeakerLayoutEditor.csproj -c Release
git add tools/SpatialAudioLab.Core/Spatial tools/SpatialAudioLab.Setup/Services tools/SpeakerLayoutEditor tools/Set-SpatialProvider.ps1 tests
git commit -m "Switch spatial providers transactionally"
```

---

### Task 9: Experimental Provider Repair and Diagnostics

**Files:**
- Create: `tools/SpatialAudioLab.Setup/Services/ExperimentalProviderRepair.cs`
- Create: `tools/SpatialAudioLab.Setup/Diagnostics/DiagnosticManifest.cs`
- Create: `tools/SpatialAudioLab.Setup/Diagnostics/DiagnosticExporter.cs`
- Create: `tests/SpatialAudioLab.Setup.Tests/ProviderRepairTests.cs`
- Create: `tests/SpatialAudioLab.Setup.Tests/DiagnosticExporterTests.cs`
- Modify: `tools/Repair-DolbySpatialProvider.ps1`

**Interfaces:**
- Repair is explicit, elevated, build-gated, backed up, verified, and reversible.
- Diagnostic export presents a manifest before archive creation.

- [ ] **Step 1: Write failing repair/export tests**

Test unsupported build refusal, binary property backup, successful verify, restore after verify
failure, media-path redaction, account-name redaction, optional detailed endpoint inclusion, and
archive cancellation.

- [ ] **Step 2: Run and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Setup.Tests\SpatialAudioLab.Setup.Tests.csproj `
  --filter "FullyQualifiedName~ProviderRepair|FullyQualifiedName~DiagnosticExporter"
```

- [ ] **Step 3: Isolate existing private property logic**

Move the known MMDevices value names/templates behind `ExperimentalProviderRepair`. Require an exact
supported Windows-build allowlist, write a timestamped binary backup below ProgramData, validate
provider and format after changes, and restore every original value on failure.

- [ ] **Step 4: Implement reviewable diagnostics**

Build a manifest of versions, readiness, owned PnP state, endpoint capabilities, profile IDs, and
logs. Redact media paths and account names by default. Show selectable optional items before writing
ZIP.

- [ ] **Step 5: Run tests and commit**

```powershell
dotnet test .\SpatialAudioLab.sln
git add tools/SpatialAudioLab.Setup tools/Repair-DolbySpatialProvider.ps1 tests
git commit -m "Add reversible provider repair diagnostics"
```

---

### Task 10: Setup Lifecycle Acceptance Verification

**Files:**
- Create: `docs/development/setup-driver-smoke-test.md`
- Modify: `driver/README.md`
- Modify: `README.md`

- [ ] **Step 1: Run all nonhardware tests**

```powershell
dotnet test .\SpatialAudioLab.sln -c Release
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
Invoke-Pester .\tests\package -Output Detailed
```

- [ ] **Step 2: Validate the complete temporary-signature flow**

From a normal boot, open Setup, verify deferral, request advanced startup, select **Disable driver
signature enforcement**, resume, install, open the capture device, launch Studio, then reboot
normally and verify Studio offers the guided flow again. Do not change TESTSIGNING/Secure Boot.

- [ ] **Step 3: Validate repair and uninstall**

Exercise provider failure rollback, driver repair, diagnostic preview/export, profile-preserving
uninstall, and full uninstall. Confirm unrelated audio endpoints/certificates remain.

- [ ] **Step 4: Document exact results and commit**

```bash
git add README.md driver/README.md docs/development/setup-driver-smoke-test.md
git commit -m "Document portable driver lifecycle"
```
