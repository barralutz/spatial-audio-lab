# First-Run Onboarding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a new user discover audio endpoints, choose or customize a 2.0-to-7.1.4 layout,
identify physical channels with test tones, and save a valid active profile without editing files.

**Architecture:** Implement onboarding as a testable state machine and plain view models, with WPF
views containing only binding and navigation. Keep endpoint proposal and assignment validation in
the shared Core library. Use a dedicated native CLI channel-test command for physical discovery.

**Tech Stack:** C# 12/.NET 8 WPF, xUnit, C++20/WASAPI, existing SpatialAudioLab Core and Engine.

## Global Constraints

- This plan starts only after the portable runtime foundation is green.
- The first-run wizard appears only when no valid active profile exists.
- Defaults provide positions, zero trim, and zero delay; no microphone calibration is added.
- No tone plays until the user explicitly presses a test button.
- Test gain defaults to 0.08, is clamped to 0..0.25, and fades in/out to avoid clicks.
- Production behavior is written only after its failing automated test has been observed.

---

### Task 1: Onboarding State Machine

**Files:**
- Create: `tools/SpeakerLayoutEditor/Onboarding/OnboardingStep.cs`
- Create: `tools/SpeakerLayoutEditor/Onboarding/OnboardingSession.cs`
- Create: `tests/SpatialAudioLab.Studio.Tests/SpatialAudioLab.Studio.Tests.csproj`
- Create: `tests/SpatialAudioLab.Studio.Tests/OnboardingSessionTests.cs`
- Modify: `SpatialAudioLab.sln`

**Interfaces:**
- Produces: `OnboardingSession` with `CurrentStep`, `CanGoBack`, `CanContinue`, `GoBack`, and
  `Continue`.
- Steps are `Readiness`, `Layout`, `Endpoints`, `Assignments`, and `Verify`.

- [ ] **Step 1: Write failing transition tests**

Cover blocked continuation without a layout, back navigation, endpoint refresh preserving layout,
assignment completion, and final profile creation:

```csharp
[Fact]
public void Session_cannot_leave_layout_without_selection() {
    OnboardingSession session = ReadySession();
    session.Continue();
    Assert.Equal(OnboardingStep.Layout, session.CurrentStep);
    Assert.False(session.CanContinue);
}
```

- [ ] **Step 2: Run tests and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Studio.Tests\SpatialAudioLab.Studio.Tests.csproj
```

- [ ] **Step 3: Implement explicit guarded transitions**

Do not encode progression in WPF button handlers. `Continue` throws only for programmer misuse;
ordinary incomplete input sets `CanContinue=false`. The session owns selected preset/custom
speakers, endpoint descriptors, proposed routes, assignments, and verification state.

- [ ] **Step 4: Run tests and commit**

```powershell
dotnet test .\tests\SpatialAudioLab.Studio.Tests\SpatialAudioLab.Studio.Tests.csproj
git add tools/SpeakerLayoutEditor/Onboarding tests/SpatialAudioLab.Studio.Tests SpatialAudioLab.sln
git commit -m "Add first-run onboarding state machine"
```

---

### Task 2: Layout Selection and Custom Layout Builder

**Files:**
- Create: `tools/SpeakerLayoutEditor/Onboarding/LayoutSelectionViewModel.cs`
- Create: `tools/SpeakerLayoutEditor/Onboarding/LayoutSelectionView.xaml`
- Create: `tools/SpeakerLayoutEditor/Onboarding/LayoutSelectionView.xaml.cs`
- Create: `tests/SpatialAudioLab.Studio.Tests/LayoutSelectionViewModelTests.cs`

**Interfaces:**
- Consumes: `LayoutPresetCatalog` and `SpeakerCatalog`.
- Produces: a validated preset clone or custom `ProfileDocument` layout skeleton.

- [ ] **Step 1: Write failing view-model tests**

Assert preset grouping into conventional/spatial, height variant selection, custom speaker toggle,
and validation messages for fewer than two speakers, missing FL/FR, over-capacity, and excess
height speakers. A preset selection must clone definitions rather than mutate the catalog.

- [ ] **Step 2: Run tests and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Studio.Tests\SpatialAudioLab.Studio.Tests.csproj `
  --filter FullyQualifiedName~LayoutSelection
```

- [ ] **Step 3: Implement the view model**

Expose `ObservableCollection<LayoutChoice> Presets`, `ObservableCollection<SpeakerChoice> Custom`,
`SelectedPreset`, `IsCustom`, `ValidationMessage`, and `BuildSpeakers()`. Use the Core catalog as the
single source of names and positions.

- [ ] **Step 4: Implement the WPF layout page**

Use a compact preset list, a segmented preset/custom selector, and checkboxes for custom speakers.
Show layout counts and validation next to the selection. Do not add explanatory marketing copy or
nested cards.

- [ ] **Step 5: Run tests/build and commit**

```powershell
dotnet test .\tests\SpatialAudioLab.Studio.Tests\SpatialAudioLab.Studio.Tests.csproj
dotnet build .\tools\SpeakerLayoutEditor\SpeakerLayoutEditor.csproj -c Release
git add tools/SpeakerLayoutEditor/Onboarding tests/SpatialAudioLab.Studio.Tests
git commit -m "Add speaker topology selection"
```

---

### Task 3: Endpoint Route Proposal

**Files:**
- Create: `tools/SpatialAudioLab.Core/Audio/RouteProposal.cs`
- Create: `tools/SpatialAudioLab.Core/Audio/RoutePlanner.cs`
- Create: `tests/SpatialAudioLab.Core.Tests/RoutePlannerTests.cs`
- Create: `tools/SpeakerLayoutEditor/Onboarding/EndpointSelectionViewModel.cs`
- Create: `tools/SpeakerLayoutEditor/Onboarding/EndpointSelectionView.xaml`
- Create: `tools/SpeakerLayoutEditor/Onboarding/EndpointSelectionView.xaml.cs`

**Interfaces:**
- Produces: `RoutePlanner.Propose(IReadOnlyList<SpeakerDefinition>,
  IReadOnlyList<AudioEndpointDescriptor>)`.
- Result contains selected master, endpoint channel slots, unassigned speakers, and warnings.

- [ ] **Step 1: Write failing route-planner tests**

Cover one 8-channel endpoint for 5.1, one 8-channel plus two stereo endpoints for 7.1.4, three
stereo endpoints for 5.1, insufficient capacity, inactive endpoint exclusion, and deterministic
tie-breaking. Prefer the fewest endpoints, then the largest endpoint as master, then stable name/ID.

- [ ] **Step 2: Run tests and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Core.Tests\SpatialAudioLab.Core.Tests.csproj `
  --filter FullyQualifiedName~RoutePlannerTests
```

- [ ] **Step 3: Implement deterministic proposal**

Only endpoints with exact exclusive 48 kHz PCM and at least two channels are eligible. Never assign
more slots than `MaximumChannels48k`. The proposal fills bed speakers on the largest endpoint and
height speakers on added stereo endpoints, but remains editable.

- [ ] **Step 4: Implement endpoint page**

Show endpoint name, ID suffix, channel count, 48 kHz status, selected/master state, and refresh.
Allow the user to accept the proposal or choose endpoints manually. Keep unavailable configured
endpoints visible with a warning.

- [ ] **Step 5: Run tests/build and commit**

```powershell
dotnet test .\SpatialAudioLab.sln
dotnet build .\tools\SpeakerLayoutEditor\SpeakerLayoutEditor.csproj -c Release
git add tools/SpatialAudioLab.Core/Audio tools/SpeakerLayoutEditor/Onboarding tests
git commit -m "Propose portable multi-endpoint routes"
```

---

### Task 4: Isolated Physical Channel Test Command

**Files:**
- Create: `src/channel_test.h`
- Create: `src/channel_test.cpp`
- Create: `tests/native/channel_test_tests.cpp`
- Modify: `src/dolby_probe.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces CLI command:
  `test-output-channel <seconds> <endpoint-id> <channels> <channel-index> <gain>`.

- [ ] **Step 1: Write failing native envelope/buffer tests**

Test argument validation, exact interleaving, silence in every nonselected channel, 500 Hz tone in
the selected channel, and a 10 ms linear fade at both edges.

- [ ] **Step 2: Build tests and verify RED**

```powershell
cmake --build build-tests --config Release --target SpatialAudioLab.NativeTests
ctest --test-dir build-tests -C Release --output-on-failure
```

- [ ] **Step 3: Implement test signal and WASAPI render**

Validate `0 < seconds <= 10`, `2 <= channels <= 12`, `channel-index < channels`, and
`0 < gain <= .25`. Select endpoint by exact ID, require exact exclusive PCM 16-bit/48 kHz for the
requested channel count, prefill one period, render the shaped signal, drain, and stop.

- [ ] **Step 4: Build and manually test at low gain**

```powershell
.\tools\Build-DolbyProbe.ps1
.\build\SpatialAudioLab.CLI.exe test-output-channel 1 '<endpoint-id>' 2 0 0.03
```

Expected: only physical channel zero emits the tone.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/channel_test.* src/dolby_probe.cpp tests/native
git commit -m "Add isolated physical channel testing"
```

---

### Task 5: Assignment and Verification Pages

**Files:**
- Create: `tools/SpeakerLayoutEditor/Onboarding/ChannelAssignmentViewModel.cs`
- Create: `tools/SpeakerLayoutEditor/Onboarding/ChannelAssignmentView.xaml`
- Create: `tools/SpeakerLayoutEditor/Onboarding/ChannelAssignmentView.xaml.cs`
- Create: `tools/SpeakerLayoutEditor/Onboarding/VerificationViewModel.cs`
- Create: `tools/SpeakerLayoutEditor/Onboarding/VerificationView.xaml`
- Create: `tests/SpatialAudioLab.Studio.Tests/ChannelAssignmentViewModelTests.cs`

**Interfaces:**
- Consumes: proposed routes and CLI physical-channel test command.
- Produces: complete `OutputRouteDefinition` list and verified `ProfileDocument`.

- [ ] **Step 1: Write failing assignment tests**

Cover swapping logical speakers, leaving unused physical slots, preventing duplicate logical
assignments, missing required speakers, endpoint removal, test command construction, and sequential
verification order.

- [ ] **Step 2: Run tests and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Studio.Tests\SpatialAudioLab.Studio.Tests.csproj `
  --filter "FullyQualifiedName~ChannelAssignment|FullyQualifiedName~Verification"
```

- [ ] **Step 3: Implement assignment view model and page**

Represent each physical slot as `(endpoint, channelIndex, physicalName, logicalSpeaker?)`. A test
button invokes one second at gain 0.08. The logical-speaker selector includes `Unused`. Assigning a
speaker already in use swaps its prior slot to `Unused` rather than duplicating it.

- [ ] **Step 4: Implement verification page**

Show the final routing table and one icon button per speaker for manual replay. Sequential test
runs one-second tones with 250 ms silence and can be stopped. Save remains disabled until every
logical speaker has one route and the profile validates.

- [ ] **Step 5: Run tests/build and commit**

```powershell
dotnet test .\SpatialAudioLab.sln
dotnet build .\tools\SpeakerLayoutEditor\SpeakerLayoutEditor.csproj -c Release
git add tools/SpeakerLayoutEditor/Onboarding tests/SpatialAudioLab.Studio.Tests
git commit -m "Add guided speaker channel assignment"
```

---

### Task 6: Wizard Shell and First-Run Activation

**Files:**
- Create: `tools/SpeakerLayoutEditor/Onboarding/OnboardingWindow.xaml`
- Create: `tools/SpeakerLayoutEditor/Onboarding/OnboardingWindow.xaml.cs`
- Modify: `tools/SpeakerLayoutEditor/App.xaml.cs`
- Modify: `tools/SpeakerLayoutEditor/MainWindow.xaml.cs`
- Create: `tests/SpatialAudioLab.Studio.Tests/FirstRunCoordinatorTests.cs`

**Interfaces:**
- Produces: `FirstRunCoordinator.RunAsync()` returning active profile or cancellation.

- [ ] **Step 1: Write failing coordinator tests**

Assert an existing valid active profile skips onboarding; missing, corrupt, or unresolved active
profiles open onboarding; cancellation exits without creating a profile; completion atomically
saves and activates the profile.

- [ ] **Step 2: Run tests and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Studio.Tests\SpatialAudioLab.Studio.Tests.csproj `
  --filter FullyQualifiedName~FirstRunCoordinatorTests
```

- [ ] **Step 3: Implement wizard shell**

Use a fixed left step list and one content region, with Back, Continue, and Cancel commands. Keep
headings compact, controls stable in size, and no nested cards. Closing before completion behaves
as cancellation.

- [ ] **Step 4: Integrate application startup**

Create runtime paths and repository, resolve active endpoints, run onboarding when required, then
construct MainWindow with the active profile. Do not load `configs/realtek-c1u-714.ini`.

- [ ] **Step 5: Run tests/build and commit**

```powershell
dotnet test .\SpatialAudioLab.sln
dotnet build .\tools\SpeakerLayoutEditor\SpeakerLayoutEditor.csproj -c Release
git add tools/SpeakerLayoutEditor tests/SpatialAudioLab.Studio.Tests
git commit -m "Launch guided first-run setup"
```

---

### Task 7: Profile Management in Studio

**Files:**
- Create: `tools/SpeakerLayoutEditor/Profiles/ProfileManagerViewModel.cs`
- Create: `tools/SpeakerLayoutEditor/Profiles/ProfileManagerWindow.xaml`
- Create: `tools/SpeakerLayoutEditor/Profiles/ProfileManagerWindow.xaml.cs`
- Modify: `tools/SpeakerLayoutEditor/MainWindow.xaml`
- Modify: `tools/SpeakerLayoutEditor/MainWindow.xaml.cs`
- Create: `tests/SpatialAudioLab.Studio.Tests/ProfileManagerViewModelTests.cs`

**Interfaces:**
- Consumes: `ProfileRepository`.
- Produces create, duplicate, rename, import, export, activate, and delete commands.

- [ ] **Step 1: Write failing command tests**

Test UUID regeneration on duplicate, no overwrite on import collision, active-profile protection on
delete, explicit confirmation for deleting the active profile, and export preserving schema v2.

- [ ] **Step 2: Run tests and verify RED**

```powershell
dotnet test .\tests\SpatialAudioLab.Studio.Tests\SpatialAudioLab.Studio.Tests.csproj `
  --filter FullyQualifiedName~ProfileManager
```

- [ ] **Step 3: Implement manager and integrate toolbar command**

Use a table with name, layout, endpoint readiness, and active status. Use familiar add, duplicate,
import, export, delete, and activate icons with tooltips. Creating a profile launches onboarding at
the layout step.

- [ ] **Step 4: Run tests/build and commit**

```powershell
dotnet test .\SpatialAudioLab.sln
dotnet build .\tools\SpeakerLayoutEditor\SpeakerLayoutEditor.csproj -c Release
git add tools/SpeakerLayoutEditor/Profiles tools/SpeakerLayoutEditor/MainWindow.* tests
git commit -m "Add portable profile management"
```

---

### Task 8: Onboarding Acceptance Verification

**Files:**
- Create: `docs/development/onboarding-smoke-test.md`
- Modify: `README.md`

- [ ] **Step 1: Run complete automated suite**

```powershell
dotnet test .\SpatialAudioLab.sln -c Release
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

- [ ] **Step 2: Run hardware smoke scenarios**

Exercise one multichannel endpoint, Realtek plus USB, and two stereo USB endpoints. Create 5.1.2,
7.1.4, and custom 3.1.2 profiles. Verify every test button isolates the chosen physical channel and
profiles survive restart.

- [ ] **Step 3: Document exact results and commit**

```bash
git add README.md docs/development/onboarding-smoke-test.md
git commit -m "Document first-run speaker configuration"
```

