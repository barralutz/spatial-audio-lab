using Microsoft.Win32;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Security.Principal;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
using IOPath = System.IO.Path;

namespace SpeakerLayoutEditor;

public sealed record EndpointChoice(string Filter, string DisplayName);

sealed record AudioEndpointInfo(string Name, string Id);

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

    readonly string repoRoot;
    readonly List<AudioEndpointInfo> activeEndpoints = [];
    readonly ObservableCollection<PhysicalDestinationChoice> physicalDestinationChoices = [];
    readonly DispatcherTimer matStatusTimer = new() {
        Interval = TimeSpan.FromSeconds(1)
    };
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
    bool matCommandRunning;
    public ObservableCollection<EndpointChoice> EndpointChoices { get; } = [];

    public MainWindow() {
        InitializeComponent();
        PhysicalDestinationCombo.ItemsSource = physicalDestinationChoices;
        repoRoot = FindRepoRoot();
        Loaded += WindowLoaded;
        Closed += (_, _) => matStatusTimer.Stop();
        matStatusTimer.Tick += (_, _) => UpdateMatStatus();
        UpdateMatControlLabels();
    }

    async void WindowLoaded(object sender, RoutedEventArgs e) {
        LoadProfile(IOPath.Combine(repoRoot, "configs", "realtek-c1u-714.ini"));
        await RefreshEndpointsAsync();
        UpdateMatStatus();
        matStatusTimer.Start();
    }

    static string FindRepoRoot() {
        DirectoryInfo? directory = new(AppContext.BaseDirectory);
        while (directory is not null) {
            if (File.Exists(IOPath.Combine(directory.FullName, "CMakeLists.txt"))) {
                return directory.FullName;
            }
            directory = directory.Parent;
        }
        throw new DirectoryNotFoundException("No se encontro la raiz de dolbyDecoder.");
    }

    void LoadProfile(string path) {
        try {
            document = LayoutDocument.Load(path);
            currentPath = IOPath.GetFullPath(path);
            SpeakerList.ItemsSource = document.Speakers;
            OutputGrid.ItemsSource = document.Outputs;
            LayoutNameText.Text = document.Name;
            PathText.Text = currentPath;
            PopulateRouteLegend();
            RebuildPhysicalDestinationChoices();
            RebuildEndpointChoices();
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
            string executable = IOPath.Combine(repoRoot, "build", "dolby-probe.exe");
            if (!File.Exists(executable)) {
                throw new FileNotFoundException("Ejecuta tools\\Build-DolbyProbe.ps1.", executable);
            }
            ProcessStartInfo start = new(executable) {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };
            start.ArgumentList.Add("list-endpoints");
            using Process process = Process.Start(start) ??
                throw new InvalidOperationException("No se pudo consultar los endpoints.");
            Task<string> outputTask = process.StandardOutput.ReadToEndAsync();
            Task<string> errorTask = process.StandardError.ReadToEndAsync();
            await process.WaitForExitAsync();
            string output = await outputTask;
            string error = await errorTask;
            if (process.ExitCode != 0) {
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(error) ? "Fallo la consulta de endpoints." : error.Trim());
            }

            activeEndpoints.Clear();
            foreach (string line in output.Split('\n', StringSplitOptions.RemoveEmptyEntries)) {
                string[] fields = line.TrimEnd('\r').Split('\t', 2);
                if (fields.Length == 2 && fields[0].Length != 0 && fields[1].Length != 0) {
                    activeEndpoints.Add(new AudioEndpointInfo(fields[0], fields[1]));
                }
            }
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

            foreach (AudioEndpointInfo endpoint in activeEndpoints) {
                string? configuredFilter = document?.Outputs
                    .Select(output => output.Endpoint)
                    .FirstOrDefault(filter => EndpointMatchesExactlyOne(filter, endpoint));
                string filter = configuredFilter ??
                    (duplicateNames.Contains(endpoint.Name) ? endpoint.Id : endpoint.Name);
                string displayName = duplicateNames.Contains(endpoint.Name)
                    ? $"{endpoint.Name} [{ShortEndpointId(endpoint.Id)}]"
                    : endpoint.Name;
                EndpointChoices.Add(new EndpointChoice(filter, displayName));
            }

            if (document is not null) {
                foreach (OutputRouteDefinition output in document.Outputs) {
                    if (EndpointChoices.Any(choice => choice.Filter.Equals(
                            output.Endpoint, StringComparison.OrdinalIgnoreCase))) continue;
                    EndpointChoices.Add(new EndpointChoice(
                        output.Endpoint, $"{output.Endpoint} (no disponible)"));
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
            comboBox.SelectedValue is not string filter) return;
        if (output.Endpoint.Equals(filter, StringComparison.OrdinalIgnoreCase)) return;
        output.Endpoint = filter;
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

    bool EndpointMatchesExactlyOne(string filter, AudioEndpointInfo candidate) {
        if (!EndpointMatches(candidate, filter)) return false;
        return activeEndpoints.Count(endpoint => EndpointMatches(endpoint, filter)) == 1;
    }

    static bool EndpointMatches(AudioEndpointInfo endpoint, string filter) =>
        endpoint.Name.Contains(filter, StringComparison.OrdinalIgnoreCase) ||
        endpoint.Id.Contains(filter, StringComparison.OrdinalIgnoreCase);

    static string ShortEndpointId(string endpointId) {
        int closingBrace = endpointId.LastIndexOf('}');
        int start = Math.Max(0, closingBrace - 8);
        return endpointId[start..Math.Max(start, closingBrace)];
    }

    void MatControlChanged(object sender, RoutedPropertyChangedEventArgs<double> e) =>
        UpdateMatControlLabels();

    void UpdateMatControlLabels() {
        if (MatGainValue is null || MatPrebufferValue is null || MatDurationValue is null) return;
        MatGainValue.Text = MatGainSlider.Value.ToString("0.00", CultureInfo.InvariantCulture);
        MatPrebufferValue.Text = $"{Math.Round(MatPrebufferSlider.Value):0} ms";
        MatDurationValue.Text = $"{Math.Round(MatDurationSlider.Value):0} min";
    }

    string MatPidPath => IOPath.Combine(repoRoot, "captures", "live-714.pid");
    string MatLogPath => IOPath.Combine(repoRoot, "captures", "live-714.log");

    bool TryGetLiveMatProcess(out Process? process) {
        process = null;
        try {
            if (!File.Exists(MatPidPath)) return false;
            string value = File.ReadAllText(MatPidPath).Trim();
            if (!int.TryParse(value, out int processId)) return false;
            process = Process.GetProcessById(processId);
            return !process.HasExited && process.ProcessName.Equals(
                "dolby-probe", StringComparison.OrdinalIgnoreCase);
        } catch (Exception error) when (error is ArgumentException or IOException or
                                            InvalidOperationException or UnauthorizedAccessException) {
            process?.Dispose();
            process = null;
            return false;
        }
    }

    void UpdateMatStatus() {
        bool running = TryGetLiveMatProcess(out Process? process);
        using (process) {
            if (!matCommandRunning) {
                MatStatusText.Text = running ? $"MAT activo · PID {process!.Id}" : "MAT detenido";
            }
        }
        MatStatusDot.Fill = new SolidColorBrush(running
            ? Color.FromRgb(55, 145, 99)
            : Color.FromRgb(139, 148, 154));
        if (!matCommandRunning) {
            MatStartButton.IsEnabled = !running;
            MatStopButton.IsEnabled = running;
        }
        MatGainSlider.IsEnabled = !running && !matCommandRunning;
        MatPrebufferSlider.IsEnabled = !running && !matCommandRunning;
        MatDurationSlider.IsEnabled = !running && !matCommandRunning;
        MatOpenLogButton.IsEnabled = File.Exists(MatLogPath);
    }

    async void StartMatClick(object sender, RoutedEventArgs e) {
        if (currentPath is null) return;
        if (TryGetLiveMatProcess(out Process? existing)) {
            existing?.Dispose();
            UpdateMatStatus();
            return;
        }
        if (!SaveProfile(currentPath)) return;
        int durationSeconds = (int)Math.Round(MatDurationSlider.Value) * 60;
        string[] arguments = [
            "-DurationSeconds", durationSeconds.ToString(CultureInfo.InvariantCulture),
            "-Gain", MatGainSlider.Value.ToString("0.###", CultureInfo.InvariantCulture),
            "-PrebufferMilliseconds",
            ((int)Math.Round(MatPrebufferSlider.Value)).ToString(CultureInfo.InvariantCulture),
            "-Layout", currentPath
        ];
        await RunMatCommandAsync("Start-Live714.ps1", arguments, "iniciar");
    }

    async void StopMatClick(object sender, RoutedEventArgs e) =>
        await RunMatCommandAsync("Stop-Live714.ps1", [], "detener");

    async Task RunMatCommandAsync(string scriptName, string[] arguments, string operation) {
        matCommandRunning = true;
        MatStartButton.IsEnabled = false;
        MatStopButton.IsEnabled = false;
        MatStatusText.Text = operation == "iniciar" ? "Iniciando MAT..." : "Deteniendo MAT...";
        try {
            ScriptResult result = await RunPowerShellScriptAsync(
                IOPath.Combine(repoRoot, "tools", scriptName), arguments);
            if (result.ExitCode != 0) {
                string detail = string.IsNullOrWhiteSpace(result.Error)
                    ? result.Output.Trim()
                    : result.Error.Trim();
                throw new InvalidOperationException(string.IsNullOrWhiteSpace(detail)
                    ? $"No se pudo {operation} MAT (codigo {result.ExitCode})."
                    : detail);
            }
            await Task.Delay(350);
            UpdateMatStatus();
            StatusText.Text = operation == "iniciar" ? "MAT iniciado" : "MAT detenido";
        } catch (Win32Exception error) when (error.NativeErrorCode == 1223) {
            StatusText.Text = "Operacion MAT cancelada";
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, $"No se pudo {operation} MAT",
                MessageBoxButton.OK, MessageBoxImage.Error);
            StatusText.Text = $"Error al {operation} MAT";
        } finally {
            matCommandRunning = false;
            UpdateMatStatus();
        }
    }

    sealed record ScriptResult(int ExitCode, string Output, string Error);

    async Task<ScriptResult> RunPowerShellScriptAsync(string scriptPath, string[] arguments) {
        bool elevated = IsElevated();
        ProcessStartInfo start = new("powershell.exe") {
            UseShellExecute = !elevated,
            WorkingDirectory = repoRoot,
            WindowStyle = ProcessWindowStyle.Hidden
        };
        if (!elevated) start.Verb = "runas";
        if (elevated) {
            start.CreateNoWindow = true;
            start.RedirectStandardOutput = true;
            start.RedirectStandardError = true;
            start.StandardOutputEncoding = Encoding.UTF8;
            start.StandardErrorEncoding = Encoding.UTF8;
        }
        start.ArgumentList.Add("-NoProfile");
        start.ArgumentList.Add("-ExecutionPolicy");
        start.ArgumentList.Add("Bypass");
        start.ArgumentList.Add("-File");
        start.ArgumentList.Add(scriptPath);
        foreach (string argument in arguments) start.ArgumentList.Add(argument);

        using Process process = Process.Start(start) ??
            throw new InvalidOperationException("No se pudo iniciar PowerShell.");
        if (!elevated) {
            await process.WaitForExitAsync();
            return new ScriptResult(process.ExitCode, "", "");
        }
        Task<string> outputTask = process.StandardOutput.ReadToEndAsync();
        Task<string> errorTask = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        return new ScriptResult(process.ExitCode, await outputTask, await errorTask);
    }

    static bool IsElevated() {
        using WindowsIdentity identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(
            WindowsBuiltInRole.Administrator);
    }

    void OpenMatLogClick(object sender, RoutedEventArgs e) {
        if (!File.Exists(MatLogPath)) return;
        Process.Start(new ProcessStartInfo(MatLogPath) { UseShellExecute = true });
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
            InitialDirectory = IOPath.Combine(repoRoot, "configs")
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
            InitialDirectory = IOPath.Combine(repoRoot, "configs"),
            FileName = currentPath is null ? "speaker-layout.ini" : IOPath.GetFileName(currentPath)
        };
        if (dialog.ShowDialog(this) == true) SaveProfile(dialog.FileName);
    }

    bool SaveProfile(string path) {
        if (document is null) return false;
        try {
            document.Save(path);
            currentPath = IOPath.GetFullPath(path);
            PathText.Text = currentPath;
            StatusText.Text = "Perfil guardado";
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
        string executable = IOPath.Combine(repoRoot, "build", "dolby-probe.exe");
        if (!File.Exists(executable)) {
            MessageBox.Show(this, "Ejecuta tools\\Build-DolbyProbe.ps1.",
                "Falta dolby-probe.exe", MessageBoxButton.OK, MessageBoxImage.Warning);
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
