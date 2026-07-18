# Portable Runtime Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Studio, Cinema, and the native Engine run from an installed or portable application
root with versioned hardware-independent profiles and layouts from 2.0 through 7.1.4.

**Architecture:** Add a platform-neutral .NET core library for runtime paths, profiles, presets, and
endpoint matching. Keep INI as the Engine interchange format, but own parsing and atomic writes in
the shared library. Extend the native Engine to read schema v2 and downmix its fixed 7.1.4 source
into any valid configured layout.

**Tech Stack:** C# 12/.NET 8, xUnit, C++20, CMake/CTest, WPF, WASAPI/MMDevice.

## Global Constraints

- Windows 11 x64 remains the only runtime target.
- Installed data lives in `%LocalAppData%\SpatialAudioLab`; portable data lives in `Data` beside
  `portable.flag`.
- Profiles support 2 through 12 logical speakers and never exceed the canonical 7.1.4 catalog.
- Existing schema v1 profiles remain importable.
- No application may require `CMakeLists.txt`, `build`, `captures`, WSL, or a repository root.
- Production behavior is written only after its failing automated test has been observed.

---

### Task 1: Shared Runtime Path Resolution

**Files:**
- Create: `tools/SpatialAudioLab.Core/SpatialAudioLab.Core.csproj`
- Create: `tools/SpatialAudioLab.Core/Runtime/RuntimePaths.cs`
- Create: `tests/SpatialAudioLab.Core.Tests/SpatialAudioLab.Core.Tests.csproj`
- Create: `tests/SpatialAudioLab.Core.Tests/RuntimePathsTests.cs`
- Create: `SpatialAudioLab.sln`

**Interfaces:**
- Produces: `SpatialAudioLab.Core.Runtime.RuntimePaths.Resolve(string applicationRoot,
  string localAppDataRoot)`.
- Produces immutable paths for `ApplicationRoot`, `DataRoot`, `ProfilesRoot`, `LogsRoot`,
  `CacheRoot`, `EngineExecutable`, `FfprobeExecutable`, `MpvExecutable`, `TrueHdExecutable`, and
  `IsPortable`.

- [ ] **Step 1: Create the solution, core project, and failing path tests**

Use `net8.0` for the core project and xUnit 2.9.2 with `Microsoft.NET.Test.Sdk` 17.12.0 for tests.
The tests must cover both modes:

```csharp
[Fact]
public void Portable_flag_keeps_mutable_data_beside_application() {
    using TemporaryDirectory root = new();
    File.WriteAllText(Path.Combine(root.Path, "portable.flag"), "");

    RuntimePaths paths = RuntimePaths.Resolve(root.Path, @"C:\Users\Test\AppData\Local");

    Assert.True(paths.IsPortable);
    Assert.Equal(Path.Combine(root.Path, "Data"), paths.DataRoot);
    Assert.Equal(Path.Combine(root.Path, "Engine", "SpatialAudioLab.CLI.exe"),
        paths.EngineExecutable);
}

[Fact]
public void Installed_mode_uses_local_app_data() {
    using TemporaryDirectory root = new();

    RuntimePaths paths = RuntimePaths.Resolve(root.Path, @"C:\Users\Test\AppData\Local");

    Assert.False(paths.IsPortable);
    Assert.Equal(@"C:\Users\Test\AppData\Local\SpatialAudioLab", paths.DataRoot);
}
```

- [ ] **Step 2: Run the tests and verify RED**

Run:

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj `
  --filter FullyQualifiedName~RuntimePathsTests
```

Expected: compilation fails because `RuntimePaths` does not exist.

- [ ] **Step 3: Implement the immutable resolver**

Implement this public shape:

```csharp
public sealed record RuntimePaths(
    string ApplicationRoot,
    string DataRoot,
    string ProfilesRoot,
    string LogsRoot,
    string CacheRoot,
    string EngineExecutable,
    string FfprobeExecutable,
    string MpvExecutable,
    string TrueHdExecutable,
    bool IsPortable) {
    public static RuntimePaths Resolve(string applicationRoot, string localAppDataRoot);
    public static RuntimePaths ResolveForCurrentProcess();
    public void EnsureUserDirectories();
}
```

Normalize all roots with `Path.GetFullPath`. `ResolveForCurrentProcess` uses
`AppContext.BaseDirectory` and `Environment.SpecialFolder.LocalApplicationData`. Tool paths are
always below `Tools`; Engine is always below `Engine`. `EnsureUserDirectories` creates only data,
profiles, logs, and cache directories.

- [ ] **Step 4: Run the focused and complete test projects**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj
```

Expected: all tests pass with zero warnings.

- [ ] **Step 5: Commit**

```bash
git add SpatialAudioLab.sln tools/SpatialAudioLab.Core tests/SpatialAudioLab.Core.Tests
git commit -m "Add portable runtime path resolution"
```

---

### Task 2: Versioned Profile Model and Atomic INI Storage

**Files:**
- Create: `tools/SpatialAudioLab.Core/Profiles/ProfileDocument.cs`
- Create: `tools/SpatialAudioLab.Core/Profiles/ProfileIniSerializer.cs`
- Create: `tools/SpatialAudioLab.Core/Profiles/ProfileRepository.cs`
- Create: `tests/SpatialAudioLab.Core.Tests/ProfileIniSerializerTests.cs`
- Create: `tests/SpatialAudioLab.Core.Tests/ProfileRepositoryTests.cs`

**Interfaces:**
- Produces: `ProfileDocument`, `SpeakerDefinition`, `OutputRouteDefinition`, `EndpointIdentity`.
- Produces: `ProfileIniSerializer.Load`, `Save`, and `ImportVersion1`.
- Produces: `ProfileRepository.List`, `LoadActive`, `Save`, `SetActive`, and `Delete`.

- [ ] **Step 1: Write failing schema v2 round-trip and v1 import tests**

The round-trip fixture must include endpoint identity and physical routing:

```ini
[profile]
version=2
id=11111111-2222-3333-4444-555555555555
name=Living room 5.1.2
layout=5.1.2-top-middle

[layout]
speakers=FL,FR,FC,LFE,SL,SR,TML,TMR
outputs=main,height
master=main

[output.main]
endpoint_id={0.0.0.00000000}.{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}
endpoint_name=Speakers
expected_channels=6
speakers=FL,FR,FC,LFE,SL,SR
delay_ms=0
```

Assert exact preservation of UUID, layout ID, endpoint ID/name, expected channels, speaker order,
position, trim, and delay. Import `configs/realtek-c1u-714.ini` and assert schema version 2 is produced
without modifying the source file.

- [ ] **Step 2: Run the profile tests and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj `
  --filter "FullyQualifiedName~Profile"
```

Expected: compilation fails because profile types are absent.

- [ ] **Step 3: Implement the model and managed INI parser**

Use these public records/classes:

```csharp
public sealed record EndpointIdentity(string Id, string FriendlyName,
                                      string? ContainerId, int ExpectedChannels);
public sealed record SpeakerDefinition(string Name, double Azimuth,
                                       double Elevation, double TrimDb);
public sealed record OutputRouteDefinition(string Name, EndpointIdentity Endpoint,
                                           IReadOnlyList<string> Speakers,
                                           double DelayMilliseconds);
public sealed class ProfileDocument {
    public const int CurrentVersion = 2;
    public required Guid Id { get; init; }
    public required string Name { get; set; }
    public required string LayoutId { get; init; }
    public required string MasterOutput { get; set; }
    public required List<SpeakerDefinition> Speakers { get; init; }
    public required List<OutputRouteDefinition> Outputs { get; init; }
    public void Validate();
}
```

The parser accepts LF or CRLF, ignores blank lines and `;`/`#` comments, rejects duplicate section
keys, and emits UTF-8 without BOM and LF endings. Validation rejects duplicate speakers, duplicate
endpoint IDs, missing assignments, duplicate assignments, routes with more channels than
`ExpectedChannels`, non-finite values, elevation outside -90..90, trim outside -60..12 dB, and
delay outside 0..500 ms.

- [ ] **Step 4: Implement atomic repository writes**

Write `<id>.ini.tmp` in the profiles directory, flush it, then call `File.Move(temp, final, true)`.
Store the active UUID in `active-profile.txt` with the same temporary-file pattern. Do not delete a
corrupt profile; return it from `List` with an error field and exclude it from `LoadActive`.

- [ ] **Step 5: Run all core tests and verify GREEN**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj
```

Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add tools/SpatialAudioLab.Core/Profiles tests/SpatialAudioLab.Core.Tests
git commit -m "Add versioned portable speaker profiles"
```

---

### Task 3: Layout Presets and Custom Topology Validation

**Files:**
- Create: `tools/SpatialAudioLab.Core/Profiles/SpeakerCatalog.cs`
- Create: `tools/SpatialAudioLab.Core/Profiles/LayoutPresetCatalog.cs`
- Create: `tests/SpatialAudioLab.Core.Tests/LayoutPresetCatalogTests.cs`

**Interfaces:**
- Produces: `SpeakerCatalog.Canonical714`.
- Produces: `LayoutPresetCatalog.All`, `Get(string id)`, and `CreateCustom`.

- [ ] **Step 1: Write failing catalog tests**

Assert that IDs are unique and that these presets exist: `2.0`, `2.1`, `3.1`, `4.0`, `4.1`,
`5.1`, `7.1`, `5.1.2-top-front`, `5.1.2-top-middle`, `5.1.2-top-rear`, `5.1.4`,
`7.1.2-top-front`, `7.1.2-top-middle`, `7.1.2-top-rear`, and `7.1.4`. Assert `7.1.4` uses the
ordered canonical names:

```csharp
string[] expected = [
    "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR",
    "TFL", "TFR", "TBL", "TBR"
];
Assert.Equal(expected, LayoutPresetCatalog.Get("7.1.4").Speakers.Select(x => x.Name));
```

Assert custom `3.1.2` is accepted and 13 speakers, five height speakers, duplicate names, or one
speaker are rejected.

- [ ] **Step 2: Run and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj `
  --filter FullyQualifiedName~LayoutPresetCatalogTests
```

- [ ] **Step 3: Implement canonical positions and presets**

Use 0 degrees for center, +/-30 for front, +/-90 for side, +/-150 for back, +/-45 at 45 degrees
elevation for top-front, 0/+90 and 0/-90 logical middle positions represented as +/-90 azimuth at
90 degrees elevation, and +/-135 at 45 degrees elevation for top-back. Presets initialize every
trim and delay to zero.

`CreateCustom` accepts 2..12 speakers, at most seven non-LFE ear-level speakers, at most one LFE,
and at most four speakers with elevation >=25 degrees. It requires FL and FR.

- [ ] **Step 4: Run all core tests and verify GREEN**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj
```

- [ ] **Step 5: Commit**

```bash
git add tools/SpatialAudioLab.Core/Profiles tests/SpatialAudioLab.Core.Tests
git commit -m "Add portable spatial layout presets"
```

---

### Task 4: Stable Endpoint Matching

**Files:**
- Create: `tools/SpatialAudioLab.Core/Audio/AudioEndpointDescriptor.cs`
- Create: `tools/SpatialAudioLab.Core/Audio/EndpointMatcher.cs`
- Create: `tests/SpatialAudioLab.Core.Tests/EndpointMatcherTests.cs`
- Modify: `src/audio_platform.h`
- Modify: `src/audio_platform.cpp`
- Modify: `src/dolby_probe.cpp`

**Interfaces:**
- Produces: `EndpointMatchResult` with `Exact`, `Fallback`, `Ambiguous`, or `Missing` status.
- Produces CLI command `list-endpoints-json` encoded as UTF-8 JSON Lines.

- [ ] **Step 1: Write failing pure matcher tests**

Cover exact endpoint ID, unique container/name/capability fallback, ambiguous duplicate names, and
channel incompatibility. An ambiguous match must not return an endpoint:

```csharp
EndpointMatchResult result = EndpointMatcher.Match(stored, [first, second]);
Assert.Equal(EndpointMatchStatus.Ambiguous, result.Status);
Assert.Null(result.Endpoint);
```

- [ ] **Step 2: Run the matcher tests and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj `
  --filter FullyQualifiedName~EndpointMatcherTests
```

- [ ] **Step 3: Implement deterministic matching**

Comparison is ordinal-ignore-case. Exact ID wins even if the friendly name changed. Fallback
requires `MaximumChannels >= ExpectedChannels`; container identity is stronger than friendly name.
Return `Ambiguous` whenever more than one candidate has the best score.

- [ ] **Step 4: Add native endpoint JSON Lines output**

Extend `Endpoint` with optional container ID and maximum exact PCM channels at 48 kHz. Add:

```text
SpatialAudioLab.CLI list-endpoints-json
```

Each line contains `id`, `name`, `containerId`, `isDefault`, `maximumChannels48k`, and
`exclusivePcm48k`. Escape JSON strings correctly; do not build JSON with unescaped concatenation.

- [ ] **Step 5: Build CLI and run contract smoke test**

```powershell
.\tools\Build-DolbyProbe.ps1
.\build\SpatialAudioLab.CLI.exe list-endpoints-json |
  ForEach-Object { $_ | ConvertFrom-Json | Out-Null }
```

Expected: every non-empty output line parses as JSON.

- [ ] **Step 6: Run core tests and commit**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj
git add tools/SpatialAudioLab.Core/Audio tests/SpatialAudioLab.Core.Tests src/audio_platform.* src/dolby_probe.cpp
git commit -m "Resolve audio endpoints by persistent identity"
```

---

### Task 5: Variable-Layout Native Downmix

**Files:**
- Create: `src/layout_mix.h`
- Create: `src/layout_mix.cpp`
- Create: `tests/native/layout_mix_tests.cpp`
- Modify: `src/speaker_layout.cpp`
- Modify: `src/pcm_pipeline.cpp`
- Modify: `src/mat_pipeline.cpp`
- Modify: `src/media_foundation_probe.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `BuildCanonical714Mix(const SpeakerLayout&)` returning a 12-by-N gain matrix.
- Produces: `MixCanonical714Frame` for 16-bit and float sources.

- [ ] **Step 1: Add failing CTest cases**

Tests must prove:

- 7.1.4 is an identity matrix.
- 5.1.2 top-middle pans TFL/TBL into TML and TFR/TBR into TMR at equal power.
- Stereo folds center equally into FL/FR and surround/back by azimuth.
- Missing LFE drops LFE rather than failing profile load.
- No row exceeds unity power after normalization.

Register `SpatialAudioLab.NativeTests` with CTest and use a tiny assertion-based executable so no
new C++ test dependency is required.

- [ ] **Step 2: Configure/build tests and verify RED**

```powershell
cmake -S . -B build-tests -DBUILD_TESTING=ON
cmake --build build-tests --config Release --target SpatialAudioLab.NativeTests
ctest --test-dir build-tests -C Release --output-on-failure
```

Expected: compilation fails because `layout_mix` is absent.

- [ ] **Step 3: Implement the canonical mix matrix**

Use the canonical source order `FL,FR,FC,LFE,BL,BR,SL,SR,TFL,TFR,TBL,TBR`. Exact destination
names receive unity. Missing non-LFE sources use `PanDirection` with their canonical azimuth and
height. Missing LFE produces an all-zero row. Normalize each source row by square-root power when
its sum of squared gains exceeds one.

- [ ] **Step 4: Use the matrix in all fixed-layout inputs**

Replace `BuildChannelMap` and direct assignment in `pcm_pipeline.cpp` with matrix mixing and
saturating accumulation. Remove the unconditional LFE requirement from `LoadSpeakerLayout`; keep
MAT's LFE decode optional and discard it when no LFE speaker exists. In DTS:X spatial conversion,
use canonical position fallback instead of dropping a static object whose exact speaker is absent.

- [ ] **Step 5: Run native tests, build CLI, and verify GREEN**

```powershell
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
.\tools\Build-DolbyProbe.ps1
```

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/layout_mix.* src/speaker_layout.cpp src/pcm_pipeline.cpp src/mat_pipeline.cpp src/media_foundation_probe.cpp tests/native
git commit -m "Render canonical spatial audio to variable layouts"
```

---

### Task 6: Detach Studio From the Repository

**Files:**
- Modify: `tools/SpeakerLayoutEditor/SpeakerLayoutEditor.csproj`
- Modify: `tools/SpeakerLayoutEditor/LayoutDocument.cs`
- Modify: `tools/SpeakerLayoutEditor/MainWindow.xaml.cs`
- Create: `tools/SpeakerLayoutEditor/Services/EndpointQuery.cs`
- Create: `tools/SpeakerLayoutEditor/Services/BridgeProcessService.cs`
- Create: `tests/SpatialAudioLab.Core.Tests/BridgeCommandTests.cs`

**Interfaces:**
- Consumes: `RuntimePaths`, `ProfileRepository`, and `EndpointMatcher`.
- Produces testable `BridgeCommand` values instead of repository PowerShell command strings.

- [ ] **Step 1: Write failing bridge-command tests**

For each mode assert executable is `RuntimePaths.EngineExecutable`, duration is `0`, profile path is
explicit, log/PID files are below `RuntimePaths.LogsRoot`, and no argument contains `configs`,
`captures`, or the repository path.

- [ ] **Step 2: Run and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj `
  --filter FullyQualifiedName~BridgeCommandTests
```

- [ ] **Step 3: Reference Core and replace repository paths**

Remove `FindRepoRoot` and `repoRoot`. Construct `RuntimePaths.ResolveForCurrentProcess`, ensure its
user directories, query endpoints using `EngineExecutable list-endpoints-json`, and load the active
profile from `ProfileRepository`. If no profile exists, show a nonfatal first-run-required state;
the full wizard arrives in the next plan.

- [ ] **Step 4: Launch bridges directly**

Replace `Start-Live*.ps1` user-runtime calls with direct CLI arguments:

```text
live-layout 0 <profile> <gain> <prebuffer-ms> <latency>
live-dtsx-layout 0 <profile> <gain> <prebuffer-ms> <latency>
live-pcm-layout 0 <profile> <gain> <prebuffer-ms> <latency>
```

Keep provider switching behind an injected service boundary for the Setup plan. Use process IDs,
not global process-name termination, to stop a bridge.

- [ ] **Step 5: Build and run tests**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj
dotnet build .\tools\SpeakerLayoutEditor\SpeakerLayoutEditor.csproj -c Release
```

- [ ] **Step 6: Commit**

```bash
git add tools/SpeakerLayoutEditor tests/SpatialAudioLab.Core.Tests
git commit -m "Run Studio from portable application paths"
```

---

### Task 7: Detach Cinema From the Repository and WSL

**Files:**
- Modify: `tools/DolbyPlayer/DolbyPlayer.csproj`
- Modify: `tools/DolbyPlayer/PlayerPaths.cs`
- Modify: `tools/DolbyPlayer/MediaProbe.cs`
- Modify: `tools/DolbyPlayer/Program.cs`
- Modify: `tools/DolbyPlayer/PlayerCoordinator.cs`
- Create: `tests/SpatialAudioLab.Cinema.Tests/SpatialAudioLab.Cinema.Tests.csproj`
- Create: `tests/SpatialAudioLab.Cinema.Tests/PlayerPathsTests.cs`
- Create: `tests/SpatialAudioLab.Cinema.Tests/MediaProbeCommandTests.cs`

**Interfaces:**
- Consumes: `RuntimePaths` and active `ProfileRepository` profile.
- Uses bundled Windows `ffprobe.exe`, `mpv.exe`, and `truehd-stream.exe`.

- [ ] **Step 1: Write failing portable Cinema tests**

Assert `PlayerPaths` resolves all tools under the application root, cache below the data root, and
builds the ffprobe command with the literal Windows media path. Assert neither `wsl.exe` nor
`/mnt/` appears.

- [ ] **Step 2: Run and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Cinema.Tests\SpatialAudioLab.Cinema.Tests.csproj
```

- [ ] **Step 3: Reference Core and replace paths**

Delete `RepoRoot`, `Wsl`, and `ToWslPath`. Wrap `RuntimePaths`; set `CacheRoot` from runtime data and
validate bundled `Tools\ffmpeg\bin\ffprobe.exe`, `Tools\mpv\mpv.exe`, and
`Tools\truehdd\truehd-stream.exe`.

- [ ] **Step 4: Use active profile and generic sink discovery**

Cinema loads the active profile and selects the Virtual Sink from Setup-owned product settings,
not the literal `1 - HISENSE` default. Keep `--sink` as a diagnostic override. Replace WSL ffprobe
execution with direct Windows ffprobe execution.

- [ ] **Step 5: Run all .NET builds and tests**

```powershell
dotnet test .\SpatialAudioLab.sln
dotnet build .\tools\SpeakerLayoutEditor\SpeakerLayoutEditor.csproj -c Release
dotnet build .\tools\DolbyPlayer\DolbyPlayer.csproj -c Release
```

- [ ] **Step 6: Commit**

```bash
git add tools/DolbyPlayer tests/SpatialAudioLab.Cinema.Tests SpatialAudioLab.sln
git commit -m "Run Cinema with bundled Windows tools"
```

---

### Task 8: Runtime Foundation Verification

**Files:**
- Modify: `README.md`
- Create: `docs/development/portable-runtime-smoke-test.md`

**Interfaces:** None; this is the phase acceptance gate.

- [ ] **Step 1: Document the source-tree smoke layout**

Document a temporary staged directory containing the three application roots, Engine, Tools, and
`portable.flag`. Include commands to copy build outputs without committing them.

- [ ] **Step 2: Run complete automated verification**

```powershell
dotnet test .\SpatialAudioLab.sln -c Release
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
.\tools\Build-DolbyProbe.ps1
```

Expected: zero failing tests and all applications build.

- [ ] **Step 3: Run portable smoke checks**

Launch Studio and Cinema from a staged directory outside the repository. Verify Studio reports
first-run-required rather than loading `realtek-c1u-714.ini`; verify Cinema `inspect` succeeds using
bundled ffprobe; verify no new file appears in repository `captures` or `configs`.

- [ ] **Step 4: Commit**

```bash
git add README.md docs/development/portable-runtime-smoke-test.md
git commit -m "Document portable runtime foundation"
```

