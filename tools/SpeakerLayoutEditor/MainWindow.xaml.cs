using Microsoft.Win32;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
using SpeakerLayoutEditor.Services;
using SpatialAudioLab.Core.Audio;
using SpatialAudioLab.Core.Profiles;
using SpatialAudioLab.Core.Runtime;
using IOPath = System.IO.Path;

namespace SpeakerLayoutEditor;

public sealed record EndpointChoice(
    string Filter,
    string DisplayName,
    AudioEndpointDescriptor? Descriptor);

sealed record PhysicalDestinationChoice(OutputRouteDefinition Output,
                                        int ChannelIndex,
                                        string DisplayName);

public partial class MainWindow : Window {
    static readonly Color[] RouteColors = [
        Color.FromRgb(52, 120, 165),
        Color.FromRgb(192, 112, 50),
        Color.FromRgb(64, 143, 104),
        Color.FromRgb(143, 84, 153)
    ];
    static readonly Dictionary<string, (double Azimuth, double Elevation)> DefaultPositions =
        new(StringComparer.OrdinalIgnoreCase) {
            ["FL"] = (-30, 0), ["FR"] = (30, 0), ["FC"] = (0, 0), ["LFE"] = (0, 0),
            ["BL"] = (-150, 0), ["BR"] = (150, 0), ["SL"] = (-90, 0), ["SR"] = (90, 0),
            ["TFL"] = (-45, 45), ["TFR"] = (45, 45),
            ["TBL"] = (-135, 45), ["TBR"] = (135, 45)
        };

    readonly RuntimePaths runtimePaths;
    readonly ProfileRepository profileRepository;
    readonly EndpointQuery endpointQuery;
    readonly BridgeProcessService bridgeProcessService = new();
    readonly ISpatialProviderService spatialProviderService =
        new DeferredSpatialProviderService();
    readonly List<AudioEndpointDescriptor> activeEndpoints = [];
    readonly ObservableCollection<PhysicalDestinationChoice> physicalDestinationChoices = [];
    readonly DispatcherTimer bridgeStatusTimer = new() {
        Interval = TimeSpan.FromSeconds(1)
    };
    readonly DispatcherTimer meterTimer = new() {
        Interval = TimeSpan.FromMilliseconds(100)
    };
    readonly BridgeMeterReader meterReader = new();
    readonly Dictionary<string, ChannelMeterControl> channelMeters =
        new(StringComparer.OrdinalIgnoreCase);
    LayoutDocument? document;
    SpeakerDefinition? selectedSpeaker;
    string? currentPath;
    bool updatingControls;
    SpeakerDefinition? draggedSpeaker;
    Canvas? dragCanvas;
    Point dragStartPoint;
    Vector dragPointerOffset;
    bool dragActivated;
    bool updatingEndpointChoices;
    bool bridgeCommandRunning;
    bool bridgeModeSelectionInitialized;
    public ObservableCollection<EndpointChoice> EndpointChoices { get; } = [];

    public MainWindow() {
        runtimePaths = RuntimePaths.ResolveForCurrentProcess();
        runtimePaths.EnsureUserDirectories();
        profileRepository = new ProfileRepository(runtimePaths.ProfilesRoot);
        endpointQuery = new EndpointQuery(runtimePaths);
        InitializeComponent();
        PhysicalDestinationCombo.ItemsSource = physicalDestinationChoices;
        Loaded += WindowLoaded;
        Closed += (_, _) => {
            bridgeStatusTimer.Stop();
            meterTimer.Stop();
            meterReader.Dispose();
        };
        bridgeStatusTimer.Tick += (_, _) => UpdateBridgeStatus();
        meterTimer.Tick += (_, _) => UpdateBridgeMeters();
        UpdateBridgeControlLabels();
    }

    async void WindowLoaded(object sender, RoutedEventArgs e) {
        ProfileDocument? activeProfile = profileRepository.LoadActive();
        if (activeProfile is null) {
            StatusText.Text = "Se requiere la configuracion inicial de parlantes";
            PathText.Text = runtimePaths.ProfilesRoot;
        } else {
            LoadProfile(activeProfile);
        }
        await RefreshEndpointsAsync();
        UpdateBridgeStatus();
        bridgeStatusTimer.Start();
        meterTimer.Start();
    }

    void LoadProfile(string path) {
        try {
            LayoutDocument imported = LayoutDocument.Load(path);
            ProfileDocument profile = imported.ToProfile();
            profileRepository.Save(profile);
            profileRepository.SetActive(profile.Id);
            LoadProfile(profile);
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "No se pudo abrir el perfil",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    void LoadProfile(ProfileDocument profile) {
        try {
            document = LayoutDocument.FromProfile(profile);
            currentPath = IOPath.Combine(
                runtimePaths.ProfilesRoot,
                $"{profile.Id:D}.ini");
            SpeakerList.ItemsSource = document.Speakers;
            OutputGrid.ItemsSource = document.Outputs;
            LayoutNameText.Text = document.Name;
            PathText.Text = currentPath;
            PopulateRouteLegend();
            RebuildPhysicalDestinationChoices();
            RebuildEndpointChoices();
            RebuildBridgeMeters();
            SpeakerList.SelectedIndex = 0;
            StatusText.Text = "Perfil cargado";
            RedrawMaps();
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "No se pudo abrir el perfil",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    async void RefreshEndpointsClick(object sender, RoutedEventArgs e) =>
        await RefreshEndpointsAsync();

    async Task RefreshEndpointsAsync() {
        RefreshEndpointsButton.IsEnabled = false;
        try {
            activeEndpoints.Clear();
            activeEndpoints.AddRange(await endpointQuery.QueryAsync());
            activeEndpoints.Sort((first, second) =>
                StringComparer.CurrentCultureIgnoreCase.Compare(first.Name, second.Name));
            RebuildEndpointChoices();
            StatusText.Text = $"{activeEndpoints.Count} endpoints activos";
        } catch (Exception error) {
            RebuildEndpointChoices();
            StatusText.Text = $"No se pudieron actualizar los endpoints: {error.Message}";
        } finally {
            RefreshEndpointsButton.IsEnabled = true;
        }
    }

    void RebuildEndpointChoices() {
        updatingEndpointChoices = true;
        try {
            EndpointChoices.Clear();
            var duplicateNames = activeEndpoints
                .GroupBy(endpoint => endpoint.Name, StringComparer.OrdinalIgnoreCase)
                .Where(group => group.Count() > 1)
                .Select(group => group.Key)
                .ToHashSet(StringComparer.OrdinalIgnoreCase);

            foreach (AudioEndpointDescriptor endpoint in activeEndpoints) {
                string displayName = duplicateNames.Contains(endpoint.Name)
                    ? $"{endpoint.Name} [{ShortEndpointId(endpoint.Id)}]"
                    : endpoint.Name;
                EndpointChoices.Add(new EndpointChoice(endpoint.Id, displayName, endpoint));
            }

            if (document is not null) {
                foreach (OutputRouteDefinition output in document.Outputs) {
                    EndpointMatchResult match = EndpointMatcher.Match(
                        new EndpointIdentity(
                            output.EndpointId,
                            output.EndpointName,
                            output.ContainerId,
                            output.ExpectedChannels),
                        activeEndpoints);
                    if (match.Endpoint is not null) {
                        AudioEndpointDescriptor endpoint = match.Endpoint;
                        output.ConfigureEndpoint(
                            endpoint.Id,
                            endpoint.Name,
                            endpoint.ContainerId,
                            Math.Max(endpoint.MaximumChannels48k, output.Speakers.Count));
                    }
                    if (EndpointChoices.Any(choice => choice.Filter.Equals(
                            output.Endpoint, StringComparison.OrdinalIgnoreCase))) continue;
                    EndpointChoices.Add(new EndpointChoice(
                        output.Endpoint, $"{output.EndpointName} (no disponible)", null));
                }
            }
            OutputGrid.Items.Refresh();
        } finally {
            updatingEndpointChoices = false;
        }
    }

    void EndpointSelectionChanged(object sender, SelectionChangedEventArgs e) {
        if (updatingEndpointChoices || sender is not ComboBox comboBox ||
            comboBox.DataContext is not OutputRouteDefinition output ||
            comboBox.SelectedItem is not EndpointChoice choice ||
            choice.Descriptor is null) return;
        if (output.Endpoint.Equals(choice.Filter, StringComparison.OrdinalIgnoreCase)) return;
        AudioEndpointDescriptor endpoint = choice.Descriptor;
        output.ConfigureEndpoint(
            endpoint.Id,
            endpoint.Name,
            endpoint.ContainerId,
            Math.Max(endpoint.MaximumChannels48k, output.Speakers.Count));
        if (selectedSpeaker is not null &&
            ReferenceEquals(document?.OutputFor(selectedSpeaker.Name), output)) {
            DestinationEndpointText.Text = output.Endpoint;
        }
        StatusText.Text = $"Endpoint actualizado: {output.Name}";
    }

    void RebuildPhysicalDestinationChoices() {
        physicalDestinationChoices.Clear();
        if (document is null) return;
        foreach (OutputRouteDefinition output in document.Outputs) {
            for (int index = 0; index < output.Speakers.Count; ++index) {
                physicalDestinationChoices.Add(new PhysicalDestinationChoice(
                    output, index,
                    $"{output.Name} / {output.PhysicalChannelName(index)} (canal {index + 1})"));
            }
        }
    }

    static string ShortEndpointId(string endpointId) {
        int closingBrace = endpointId.LastIndexOf('}');
        int start = Math.Max(0, closingBrace - 8);
        return endpointId[start..Math.Max(start, closingBrace)];
    }

    BridgeMode SelectedBridgeMode =>
        PcmModeButton.IsChecked == true ? BridgeMode.Pcm :
        NativeMatModeButton.IsChecked == true ? BridgeMode.NativeMat :
        DtsXModeButton.IsChecked == true ? BridgeMode.DtsX : BridgeMode.Atmos;

    BridgeLatencyMode SelectedBridgeLatencyMode =>
        SafeLatencyButton.IsChecked == true ? BridgeLatencyMode.Safe :
        LowLatencyButton.IsChecked == true ? BridgeLatencyMode.Low :
        BridgeLatencyMode.Balanced;

    static string BridgeName(BridgeMode mode) => mode switch {
        BridgeMode.Atmos => "Dolby Atmos (Windows)",
        BridgeMode.NativeMat => "Dolby MAT nativo",
        BridgeMode.DtsX => "DTS:X",
        BridgeMode.Pcm => "PCM propio 7.1.4",
        _ => throw new ArgumentOutOfRangeException(nameof(mode))
    };

    string BridgePidPath(BridgeMode mode) => IOPath.Combine(runtimePaths.LogsRoot, mode switch {
        BridgeMode.Atmos => "bridge-atmos.pid",
        BridgeMode.NativeMat => "bridge-native-mat.pid",
        BridgeMode.DtsX => "bridge-dtsx.pid",
        BridgeMode.Pcm => "bridge-pcm.pid",
        _ => throw new ArgumentOutOfRangeException(nameof(mode))
    });

    string BridgeLogPath(BridgeMode mode) => IOPath.Combine(runtimePaths.LogsRoot, mode switch {
        BridgeMode.Atmos => "bridge-atmos.log",
        BridgeMode.NativeMat => "bridge-native-mat.log",
        BridgeMode.DtsX => "bridge-dtsx.log",
        BridgeMode.Pcm => "bridge-pcm.log",
        _ => throw new ArgumentOutOfRangeException(nameof(mode))
    });

    void BridgeControlChanged(object sender, RoutedPropertyChangedEventArgs<double> e) =>
        UpdateBridgeControlLabels();

    void BridgeLatencyChecked(object sender, RoutedEventArgs e) {
        if (BridgePrebufferSlider is null) return;
        BridgePrebufferSlider.Value = SelectedBridgeLatencyMode switch {
            BridgeLatencyMode.Safe => 80,
            BridgeLatencyMode.Balanced => 40,
            BridgeLatencyMode.Low => 20,
            _ => throw new ArgumentOutOfRangeException()
        };
        UpdateBridgeControlLabels();
    }

    void UpdateBridgeControlLabels() {
        if (BridgeGainValue is null || BridgePrebufferValue is null) return;
        BridgeGainValue.Text = BridgeGainSlider.Value.ToString("0.00", CultureInfo.InvariantCulture);
        BridgePrebufferValue.Text = $"{Math.Round(BridgePrebufferSlider.Value):0} ms";
    }

    void BridgeModeClick(object sender, RoutedEventArgs e) {
        bridgeModeSelectionInitialized = true;
        UpdateBridgeStatus();
    }

    void SelectBridgeMode(BridgeMode mode) {
        MatModeButton.IsChecked = mode == BridgeMode.Atmos;
        NativeMatModeButton.IsChecked = mode == BridgeMode.NativeMat;
        DtsXModeButton.IsChecked = mode == BridgeMode.DtsX;
        PcmModeButton.IsChecked = mode == BridgeMode.Pcm;
    }

    bool TryGetLiveBridgeProcess(BridgeMode mode, out Process? process) {
        process = null;
        try {
            string pidPath = BridgePidPath(mode);
            if (!File.Exists(pidPath)) return false;
            string value = File.ReadAllText(pidPath).Trim();
            if (!int.TryParse(value, out int processId)) return false;
            process = Process.GetProcessById(processId);
            string processName = process.ProcessName;
            if (!process.HasExited &&
                (processName.Equals("SpatialAudioLab.CLI", StringComparison.OrdinalIgnoreCase) ||
                 processName.Equals("dolby-probe", StringComparison.OrdinalIgnoreCase))) return true;
            process.Dispose();
            process = null;
            return false;
        } catch (Exception error) when (error is ArgumentException or IOException or
                                            InvalidOperationException or UnauthorizedAccessException) {
            process?.Dispose();
            process = null;
            return false;
        }
    }

    List<BridgeMode> RunningBridgeModes() {
        List<BridgeMode> result = [];
        foreach (BridgeMode mode in Enum.GetValues<BridgeMode>()) {
            if (TryGetLiveBridgeProcess(mode, out Process? process)) result.Add(mode);
            process?.Dispose();
        }
        return result;
    }

    void UpdateBridgeStatus() {
        List<BridgeMode> runningModes = RunningBridgeModes();
        BridgeMode statusMode = runningModes.Count == 1 ? runningModes[0] : SelectedBridgeMode;
        if (!bridgeModeSelectionInitialized && runningModes.Count == 1) {
            SelectBridgeMode(statusMode);
            bridgeModeSelectionInitialized = true;
        }

        if (!bridgeCommandRunning) {
            if (runningModes.Count > 1) {
                BridgeStatusText.Text = "Varios puentes activos";
            } else if (runningModes.Count == 1 &&
                       TryGetLiveBridgeProcess(statusMode, out Process? process)) {
                using (process) {
                    BridgeStatusText.Text = $"{BridgeName(statusMode)} activo · PID {process!.Id}";
                }
            } else {
                BridgeStatusText.Text = $"{BridgeName(statusMode)} detenido";
            }
        }

        bool anyRunning = runningModes.Count != 0;
        bool selectedModeRunning = runningModes.Count == 1 &&
            runningModes[0] == SelectedBridgeMode;
        BridgeStatusDot.Fill = new SolidColorBrush(runningModes.Count > 1
            ? Color.FromRgb(178, 80, 65)
            : anyRunning
                ? Color.FromRgb(55, 145, 99)
                : Color.FromRgb(139, 148, 154));
        if (!bridgeCommandRunning) {
            BridgeStartButton.IsEnabled = !selectedModeRunning;
            BridgeStopButton.IsEnabled = anyRunning;
            BridgeStartText.Text = !anyRunning
                ? "Iniciar"
                : selectedModeRunning ? "Activo" : "Cambiar";
        }
        MatModeButton.IsEnabled = !bridgeCommandRunning;
        NativeMatModeButton.IsEnabled = !bridgeCommandRunning;
        DtsXModeButton.IsEnabled = !bridgeCommandRunning;
        PcmModeButton.IsEnabled = !bridgeCommandRunning;
        BridgeGainSlider.IsEnabled = !anyRunning && !bridgeCommandRunning;
        BridgePrebufferSlider.IsEnabled = !anyRunning && !bridgeCommandRunning;
        SafeLatencyButton.IsEnabled = !anyRunning && !bridgeCommandRunning;
        BalancedLatencyButton.IsEnabled = !anyRunning && !bridgeCommandRunning;
        LowLatencyButton.IsEnabled = !anyRunning && !bridgeCommandRunning;
        BridgeOpenLogButton.IsEnabled = File.Exists(BridgeLogPath(statusMode));
    }

    async void StartBridgeClick(object sender, RoutedEventArgs e) {
        if (currentPath is null) return;
        bool switching = RunningBridgeModes().Count != 0;
        if (!SaveProfile(currentPath)) return;

        BridgeMode mode = SelectedBridgeMode;
        string bridgeName = BridgeName(mode);
        bridgeCommandRunning = true;
        BridgeStartButton.IsEnabled = false;
        BridgeStopButton.IsEnabled = false;
        BridgeStatusText.Text = switching
            ? $"Cambiando a {bridgeName}..."
            : $"Iniciando {bridgeName}...";
        try {
            foreach (BridgeMode runningMode in RunningBridgeModes()) {
                await bridgeProcessService.StopAsync(BridgePidPath(runningMode));
            }
            await spatialProviderService.ActivateAsync(mode);
            BridgeCommand command = BridgeCommandFactory.Create(
                runtimePaths,
                mode,
                currentPath,
                BridgeGainSlider.Value,
                (int)Math.Round(BridgePrebufferSlider.Value),
                SelectedBridgeLatencyMode);
            await bridgeProcessService.StartAsync(command);
            StatusText.Text = switching
                ? $"Puente cambiado a {bridgeName}"
                : $"{bridgeName} iniciado";
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, $"No se pudo iniciar {bridgeName}",
                MessageBoxButton.OK, MessageBoxImage.Error);
            StatusText.Text = $"Error al iniciar {bridgeName}";
        } finally {
            bridgeCommandRunning = false;
            UpdateBridgeStatus();
        }
    }

    async void StopBridgeClick(object sender, RoutedEventArgs e) {
        bridgeCommandRunning = true;
        BridgeStartButton.IsEnabled = false;
        BridgeStopButton.IsEnabled = false;
        BridgeStatusText.Text = "Deteniendo puente...";
        try {
            foreach (BridgeMode mode in RunningBridgeModes()) {
                await bridgeProcessService.StopAsync(BridgePidPath(mode));
            }
            StatusText.Text = "Puente detenido";
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "No se pudo detener el puente",
                MessageBoxButton.OK, MessageBoxImage.Error);
            StatusText.Text = "Error al detener el puente";
        } finally {
            bridgeCommandRunning = false;
            UpdateBridgeStatus();
        }
    }

    void OpenBridgeLogClick(object sender, RoutedEventArgs e) {
        List<BridgeMode> runningModes = RunningBridgeModes();
        BridgeMode mode = runningModes.Count == 1 ? runningModes[0] : SelectedBridgeMode;
        string logPath = BridgeLogPath(mode);
        if (!File.Exists(logPath)) return;
        Process.Start(new ProcessStartInfo(logPath) { UseShellExecute = true });
    }

    void OpenSpatialSettingsClick(object sender, RoutedEventArgs e) {
        try {
            Process.Start(new ProcessStartInfo("ms-settings:sound") {
                UseShellExecute = true
            });
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "No se pudo abrir Sonido",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    void RebuildBridgeMeters() {
        BridgeMeterPanel.Children.Clear();
        channelMeters.Clear();
        if (document is null) return;
        foreach (SpeakerDefinition speaker in document.Speakers) {
            ChannelMeterControl meter = new(speaker.Name);
            channelMeters.Add(speaker.Name, meter);
            BridgeMeterPanel.Children.Add(meter);
        }
    }

    void UpdateBridgeMeters() {
        if (!meterReader.TryRead(out BridgeMeterSnapshot? snapshot) ||
            snapshot is null || !snapshot.IsFresh) {
            ResetBridgeMeters();
            return;
        }

        HashSet<string> updated = new(StringComparer.OrdinalIgnoreCase);
        foreach (BridgeMeterChannel channel in snapshot.Channels) {
            if (!channelMeters.TryGetValue(channel.Name, out ChannelMeterControl? meter)) continue;
            meter.SetLevel(channel.Rms, channel.Peak);
            updated.Add(channel.Name);
        }
        foreach ((string name, ChannelMeterControl meter) in channelMeters) {
            if (!updated.Contains(name)) meter.Reset();
        }
    }

    void ResetBridgeMeters() {
        foreach (ChannelMeterControl meter in channelMeters.Values) meter.Reset();
    }

    void PopulateRouteLegend() {
        RouteLegend.Children.Clear();
        if (document is null) return;
        for (int index = 0; index < document.Outputs.Count; ++index) {
            RouteLegend.Children.Add(new Rectangle {
                Width = 12,
                Height = 12,
                Fill = new SolidColorBrush(RouteColors[index % RouteColors.Length]),
                Margin = new Thickness(0, 0, 5, 0)
            });
            RouteLegend.Children.Add(new TextBlock {
                Text = document.Outputs[index].Name,
                Margin = new Thickness(0, 0, 12, 0)
            });
        }
    }

    void OpenClick(object sender, RoutedEventArgs e) {
        OpenFileDialog dialog = new() {
            Filter = "Perfil de parlantes (*.ini)|*.ini|Todos los archivos (*.*)|*.*",
            InitialDirectory = runtimePaths.ProfilesRoot
        };
        if (dialog.ShowDialog(this) == true) LoadProfile(dialog.FileName);
    }

    void SaveClick(object sender, RoutedEventArgs e) {
        if (currentPath is null) {
            SaveAsClick(sender, e);
            return;
        }
        SaveProfile(currentPath);
    }

    void SaveAsClick(object sender, RoutedEventArgs e) {
        SaveFileDialog dialog = new() {
            Filter = "Perfil de parlantes (*.ini)|*.ini",
            InitialDirectory = runtimePaths.ProfilesRoot,
            FileName = currentPath is null ? "speaker-layout.ini" : IOPath.GetFileName(currentPath)
        };
        if (dialog.ShowDialog(this) == true) SaveProfile(dialog.FileName);
    }

    bool SaveProfile(string path) {
        if (document is null) return false;
        try {
            ProfileDocument profile = document.ToProfile();
            profileRepository.Save(profile);
            profileRepository.SetActive(profile.Id);
            string canonicalPath = IOPath.Combine(
                runtimePaths.ProfilesRoot,
                $"{profile.Id:D}.ini");
            string requestedPath = IOPath.GetFullPath(path);
            if (!requestedPath.Equals(canonicalPath, StringComparison.OrdinalIgnoreCase)) {
                ProfileIniSerializer.Save(requestedPath, profile);
            }
            currentPath = canonicalPath;
            PathText.Text = currentPath;
            StatusText.Text = requestedPath.Equals(canonicalPath, StringComparison.OrdinalIgnoreCase)
                ? "Perfil guardado"
                : $"Perfil guardado y exportado a {requestedPath}";
            return true;
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "No se pudo guardar el perfil",
                MessageBoxButton.OK, MessageBoxImage.Error);
            return false;
        }
    }

    void SpeakerSelectionChanged(object sender, SelectionChangedEventArgs e) {
        selectedSpeaker = SpeakerList.SelectedItem as SpeakerDefinition;
        UpdateSpeakerControls();
        RedrawMaps();
    }

    void UpdateSpeakerControls() {
        updatingControls = true;
        bool enabled = selectedSpeaker is not null && document is not null;
        AzimuthSlider.IsEnabled = enabled;
        ElevationSlider.IsEnabled = enabled;
        TrimSlider.IsEnabled = enabled;
        PhysicalDestinationCombo.IsEnabled = enabled;
        if (enabled) {
            SelectedNameText.Text = selectedSpeaker!.Name;
            AzimuthSlider.Value = selectedSpeaker.Azimuth;
            ElevationSlider.Value = selectedSpeaker.Elevation;
            TrimSlider.Value = selectedSpeaker.TrimDb;
            var slot = document!.SlotFor(selectedSpeaker.Name);
            PhysicalDestinationCombo.SelectedItem = slot.HasValue
                ? physicalDestinationChoices.FirstOrDefault(choice =>
                    ReferenceEquals(choice.Output, slot.Value.Output) &&
                    choice.ChannelIndex == slot.Value.ChannelIndex)
                : null;
            DestinationEndpointText.Text = slot?.Output.Endpoint ?? "Sin destino";
        } else {
            SelectedNameText.Text = "";
            PhysicalDestinationCombo.SelectedItem = null;
            DestinationEndpointText.Text = "";
        }
        UpdateValueLabels();
        updatingControls = false;
    }

    void SpeakerControlChanged(object sender, RoutedPropertyChangedEventArgs<double> e) {
        if (updatingControls || selectedSpeaker is null) return;
        selectedSpeaker.Azimuth = Math.Round(AzimuthSlider.Value, 1);
        selectedSpeaker.Elevation = Math.Round(ElevationSlider.Value, 1);
        selectedSpeaker.TrimDb = Math.Round(TrimSlider.Value, 1);
        UpdateValueLabels();
        RedrawMaps();
    }

    void UpdateValueLabels() {
        AzimuthValue.Text = $"{AzimuthSlider.Value:0.0}°";
        ElevationValue.Text = $"{ElevationSlider.Value:0.0}°";
        TrimValue.Text = $"{TrimSlider.Value:0.0} dB";
    }

    void PhysicalDestinationSelectionChanged(object sender, SelectionChangedEventArgs e) {
        if (updatingControls || document is null || selectedSpeaker is null ||
            PhysicalDestinationCombo.SelectedItem is not PhysicalDestinationChoice target) return;
        var source = document.SlotFor(selectedSpeaker.Name);
        if (!source.HasValue ||
            (ReferenceEquals(source.Value.Output, target.Output) &&
             source.Value.ChannelIndex == target.ChannelIndex)) return;

        string previousDestination =
            $"{source.Value.Output.Name} / " +
            $"{source.Value.Output.PhysicalChannelName(source.Value.ChannelIndex)}";
        string displaced = document.AssignSpeakerToSlot(
            selectedSpeaker.Name, target.Output, target.ChannelIndex);
        OutputGrid.Items.Refresh();
        UpdateSpeakerControls();
        RedrawMaps();
        StatusText.Text =
            $"{selectedSpeaker.Name} -> {target.Output.Name} / " +
            $"{target.Output.PhysicalChannelName(target.ChannelIndex)}; " +
            $"{displaced} -> {previousDestination} (sin guardar)";
    }

    void ResetPositionsClick(object sender, RoutedEventArgs e) {
        if (document is null) return;
        int reset = 0;
        foreach (SpeakerDefinition speaker in document.Speakers) {
            if (!DefaultPositions.TryGetValue(speaker.Name, out var position)) continue;
            speaker.Azimuth = position.Azimuth;
            speaker.Elevation = position.Elevation;
            ++reset;
        }
        UpdateSpeakerControls();
        RedrawMaps();
        StatusText.Text = $"{reset} posiciones restablecidas (sin guardar)";
    }

    async void TestSpeakerClick(object sender, RoutedEventArgs e) {
        if (document is null || selectedSpeaker is null || currentPath is null) return;
        if (!SaveProfile(currentPath)) return;
        string executable = runtimePaths.EngineExecutable;
        if (!File.Exists(executable)) {
            MessageBox.Show(this, $"No se encontro el Engine en:\n{executable}",
                "Falta SpatialAudioLab.CLI.exe", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        try {
            StatusText.Text = $"Probando {selectedSpeaker.Name}";
            ProcessStartInfo start = new(executable) {
                UseShellExecute = false,
                CreateNoWindow = true
            };
            start.ArgumentList.Add("test-speaker");
            start.ArgumentList.Add("3");
            start.ArgumentList.Add(currentPath);
            start.ArgumentList.Add(selectedSpeaker.Name);
            start.ArgumentList.Add(TestGainSlider.Value.ToString(
                "0.###", CultureInfo.InvariantCulture));
            using Process process = Process.Start(start) ??
                throw new InvalidOperationException("No se pudo iniciar la prueba.");
            await process.WaitForExitAsync();
            StatusText.Text = process.ExitCode == 0
                ? $"Prueba completada: {selectedSpeaker.Name}"
                : $"La prueba termino con codigo {process.ExitCode}";
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "Error de prueba",
                MessageBoxButton.OK, MessageBoxImage.Error);
            StatusText.Text = "Error de prueba";
        }
    }

    void CanvasSizeChanged(object sender, SizeChangedEventArgs e) => RedrawMaps();

    void RedrawMaps() {
        if (!IsLoaded || document is null || TopCanvas.ActualWidth < 100 ||
            ElevationCanvas.ActualWidth < 100) return;
        DrawTopView();
        DrawElevationView();
    }

    void DrawTopView() {
        TopCanvas.Children.Clear();
        double width = TopCanvas.ActualWidth;
        double height = TopCanvas.ActualHeight;
        double centerX = width / 2;
        double centerY = height / 2;
        double radius = Math.Max(40, Math.Min(width, height) * 0.39);
        TopCanvas.Children.Add(new Ellipse {
            Width = radius * 2,
            Height = radius * 2,
            Stroke = new SolidColorBrush(Color.FromRgb(201, 207, 211)),
            StrokeThickness = 1
        }.At(centerX - radius, centerY - radius));
        AddLine(TopCanvas, centerX, centerY - radius, centerX, centerY + radius);
        AddLine(TopCanvas, centerX - radius, centerY, centerX + radius, centerY);
        AddListener(TopCanvas, centerX, centerY);

        foreach (SpeakerDefinition speaker in document!.Speakers) {
            double markerRadius = speaker.Name.Equals("LFE", StringComparison.OrdinalIgnoreCase)
                ? radius * 0.18
                : speaker.Elevation >= 25 ? radius * 0.65 : radius * 0.92;
            double radians = speaker.Azimuth * Math.PI / 180;
            double x = centerX + Math.Sin(radians) * markerRadius;
            double y = centerY - Math.Cos(radians) * markerRadius;
            AddSpeakerMarker(TopCanvas, speaker, x, y);
        }
    }

    void DrawElevationView() {
        ElevationCanvas.Children.Clear();
        double width = ElevationCanvas.ActualWidth;
        double height = ElevationCanvas.ActualHeight;
        const double margin = 24;
        List<Point> occupied = new();
        double zeroY = ElevationToY(0, height, margin);
        AddLine(ElevationCanvas, margin, zeroY, width - margin, zeroY);
        AddLine(ElevationCanvas, width / 2, margin, width / 2, height - margin);
        foreach (SpeakerDefinition speaker in document!.Speakers) {
            double x = margin + (speaker.Azimuth + 180) / 360 * (width - margin * 2);
            double y = ElevationToY(speaker.Elevation, height, margin);
            for (int attempt = 0; attempt < 4 &&
                 occupied.Any(point => Math.Abs(point.X - x) < 36 &&
                                       Math.Abs(point.Y - y) < 24); ++attempt) {
                y = Math.Min(height - 18, y + 26);
            }
            occupied.Add(new Point(x, y));
            AddSpeakerMarker(ElevationCanvas, speaker, x, y);
        }
    }

    static double ElevationToY(double elevation, double height, double margin) {
        double normalized = (Math.Clamp(elevation, -30, 90) + 30) / 120;
        return height - margin - normalized * (height - margin * 2);
    }

    void AddSpeakerMarker(Canvas canvas, SpeakerDefinition speaker, double x, double y) {
        OutputRouteDefinition? route = document!.OutputFor(speaker.Name);
        int routeIndex = route is null ? 0 : document.Outputs.IndexOf(route);
        Color color = RouteColors[Math.Abs(routeIndex) % RouteColors.Length];
        bool selected = ReferenceEquals(speaker, selectedSpeaker);
        Border marker = new() {
            Width = 42,
            Height = 30,
            CornerRadius = new CornerRadius(4),
            Background = new SolidColorBrush(color),
            BorderBrush = selected ? Brushes.Black : Brushes.White,
            BorderThickness = new Thickness(selected ? 2 : 1),
            Tag = speaker,
            Cursor = Cursors.Hand,
            Child = new TextBlock {
                Text = speaker.Name,
                Foreground = Brushes.White,
                FontSize = 11,
                FontWeight = FontWeights.SemiBold,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center
            }
        };
        marker.MouseLeftButtonDown += MarkerMouseDown;
        Canvas.SetLeft(marker, x - marker.Width / 2);
        Canvas.SetTop(marker, y - marker.Height / 2);
        canvas.Children.Add(marker);
    }

    void MarkerMouseDown(object sender, MouseButtonEventArgs e) {
        if (sender is not Border marker || marker.Tag is not SpeakerDefinition speaker) return;
        draggedSpeaker = speaker;
        dragCanvas = FindParentCanvas(marker);
        if (dragCanvas is null) return;
        dragStartPoint = e.GetPosition(dragCanvas);
        Point markerCenter = new(
            Canvas.GetLeft(marker) + marker.Width / 2,
            Canvas.GetTop(marker) + marker.Height / 2);
        dragPointerOffset = dragStartPoint - markerCenter;
        dragActivated = false;
        dragCanvas?.CaptureMouse();
        SpeakerList.SelectedItem = speaker;
        e.Handled = true;
    }

    void CanvasMouseMove(object sender, MouseEventArgs e) {
        if (e.LeftButton != MouseButtonState.Pressed || draggedSpeaker is null ||
            dragCanvas is null || !ReferenceEquals(sender, dragCanvas)) return;
        Point pointer = e.GetPosition(dragCanvas);
        if (!dragActivated) {
            if (Math.Abs(pointer.X - dragStartPoint.X) < SystemParameters.MinimumHorizontalDragDistance &&
                Math.Abs(pointer.Y - dragStartPoint.Y) < SystemParameters.MinimumVerticalDragDistance) {
                return;
            }
            dragActivated = true;
        }
        Point point = pointer - dragPointerOffset;
        if (ReferenceEquals(dragCanvas, TopCanvas)) {
            double dx = point.X - TopCanvas.ActualWidth / 2;
            double dy = point.Y - TopCanvas.ActualHeight / 2;
            draggedSpeaker.Azimuth = Math.Round(Math.Atan2(dx, -dy) * 180 / Math.PI, 1);
        } else {
            const double margin = 24;
            double usableWidth = Math.Max(1, ElevationCanvas.ActualWidth - margin * 2);
            double usableHeight = Math.Max(1, ElevationCanvas.ActualHeight - margin * 2);
            draggedSpeaker.Azimuth = Math.Round(
                Math.Clamp((point.X - margin) / usableWidth, 0, 1) * 360 - 180, 1);
            double normalized = 1 - Math.Clamp((point.Y - margin) / usableHeight, 0, 1);
            draggedSpeaker.Elevation = Math.Round(normalized * 120 - 30, 1);
        }
        UpdateSpeakerControls();
        RedrawMaps();
        e.Handled = true;
    }

    void CanvasMouseUp(object sender, MouseButtonEventArgs e) {
        if (dragCanvas is null || !ReferenceEquals(sender, dragCanvas)) return;
        dragCanvas.ReleaseMouseCapture();
        draggedSpeaker = null;
        dragCanvas = null;
        dragActivated = false;
        e.Handled = true;
    }

    static Canvas? FindParentCanvas(DependencyObject child) {
        DependencyObject? current = child;
        while (current is not null) {
            if (current is Canvas canvas) return canvas;
            current = VisualTreeHelper.GetParent(current);
        }
        return null;
    }

    static void AddLine(Canvas canvas, double x1, double y1, double x2, double y2) {
        canvas.Children.Add(new Line {
            X1 = x1, Y1 = y1, X2 = x2, Y2 = y2,
            Stroke = new SolidColorBrush(Color.FromRgb(218, 222, 225)),
            StrokeThickness = 1
        });
    }

    static void AddListener(Canvas canvas, double x, double y) {
        Ellipse listener = new() {
            Width = 18, Height = 18,
            Fill = new SolidColorBrush(Color.FromRgb(37, 43, 48)),
            Stroke = Brushes.White, StrokeThickness = 2
        };
        Canvas.SetLeft(listener, x - 9);
        Canvas.SetTop(listener, y - 9);
        canvas.Children.Add(listener);
    }
}

internal static class CanvasElementExtensions {
    public static T At<T>(this T element, double left, double top) where T : UIElement {
        Canvas.SetLeft(element, left);
        Canvas.SetTop(element, top);
        return element;
    }
}
