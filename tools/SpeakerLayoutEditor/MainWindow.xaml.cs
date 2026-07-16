using Microsoft.Win32;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using IOPath = System.IO.Path;

namespace SpeakerLayoutEditor;

public partial class MainWindow : Window {
    static readonly Color[] RouteColors = [
        Color.FromRgb(52, 120, 165),
        Color.FromRgb(192, 112, 50),
        Color.FromRgb(64, 143, 104),
        Color.FromRgb(143, 84, 153)
    ];

    readonly string repoRoot;
    LayoutDocument? document;
    SpeakerDefinition? selectedSpeaker;
    string? currentPath;
    bool updatingControls;
    SpeakerDefinition? draggedSpeaker;
    Canvas? dragCanvas;

    public MainWindow() {
        InitializeComponent();
        repoRoot = FindRepoRoot();
        Loaded += (_, _) => LoadProfile(IOPath.Combine(repoRoot, "configs", "realtek-c1u-714.ini"));
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
            OutputCombo.ItemsSource = document.Outputs;
            OutputGrid.ItemsSource = document.Outputs;
            LayoutNameText.Text = document.Name;
            PathText.Text = currentPath;
            PopulateRouteLegend();
            SpeakerList.SelectedIndex = 0;
            StatusText.Text = "Perfil cargado";
            RedrawMaps();
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "No se pudo abrir el perfil",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
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
        OutputCombo.IsEnabled = enabled;
        if (enabled) {
            SelectedNameText.Text = selectedSpeaker!.Name;
            AzimuthSlider.Value = selectedSpeaker.Azimuth;
            ElevationSlider.Value = selectedSpeaker.Elevation;
            TrimSlider.Value = selectedSpeaker.TrimDb;
            OutputCombo.SelectedItem = document!.OutputFor(selectedSpeaker.Name);
        } else {
            SelectedNameText.Text = "";
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

    void OutputSelectionChanged(object sender, SelectionChangedEventArgs e) {
        if (updatingControls || document is null || selectedSpeaker is null ||
            OutputCombo.SelectedItem is not OutputRouteDefinition target) return;
        OutputRouteDefinition? current = document.OutputFor(selectedSpeaker.Name);
        if (ReferenceEquals(current, target)) return;
        current?.Speakers.RemoveAll(name => name.Equals(
            selectedSpeaker.Name, StringComparison.OrdinalIgnoreCase));
        current?.NotifySpeakersChanged();
        target.Speakers.Add(selectedSpeaker.Name);
        target.NotifySpeakersChanged();
        OutputGrid.Items.Refresh();
        RedrawMaps();
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
        dragCanvas?.CaptureMouse();
        SpeakerList.SelectedItem = speaker;
        e.Handled = true;
    }

    void CanvasMouseMove(object sender, MouseEventArgs e) {
        if (e.LeftButton != MouseButtonState.Pressed || draggedSpeaker is null ||
            dragCanvas is null || !ReferenceEquals(sender, dragCanvas)) return;
        Point point = e.GetPosition(dragCanvas);
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
