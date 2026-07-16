using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace SpeakerLayoutEditor;

internal sealed class SpeakerDefinition : INotifyPropertyChanged {
    string name = "";
    double azimuth;
    double elevation;
    double trimDb;

    public string Name { get => name; set => Set(ref name, value); }
    public double Azimuth { get => azimuth; set => Set(ref azimuth, value); }
    public double Elevation { get => elevation; set => Set(ref elevation, value); }
    public double TrimDb { get => trimDb; set => Set(ref trimDb, value); }

    public event PropertyChangedEventHandler? PropertyChanged;

    void Set<T>(ref T field, T value, [CallerMemberName] string? property = null) {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
    }
}

internal sealed class OutputRouteDefinition : INotifyPropertyChanged {
    string name = "";
    string endpoint = "";
    double delayMilliseconds;

    public string Name { get => name; set => Set(ref name, value); }
    public string Endpoint { get => endpoint; set => Set(ref endpoint, value); }
    public double DelayMilliseconds {
        get => delayMilliseconds;
        set => Set(ref delayMilliseconds, Math.Clamp(value, 0, 500));
    }
    public List<string> Speakers { get; } = new();
    public string SpeakersText => string.Join(", ", Speakers);

    public event PropertyChangedEventHandler? PropertyChanged;

    public void NotifySpeakersChanged() =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(SpeakersText)));

    void Set<T>(ref T field, T value, [CallerMemberName] string? property = null) {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
    }
}

internal sealed class LayoutDocument {
    const int BufferCharacters = 32_768;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetPrivateProfileString(
        string section, string key, string defaultValue, StringBuilder value,
        uint size, string filePath);

    public string Name { get; set; } = "";
    public string MasterOutput { get; set; } = "";
    public List<SpeakerDefinition> Speakers { get; } = new();
    public List<OutputRouteDefinition> Outputs { get; } = new();

    public static LayoutDocument Load(string path) {
        path = Path.GetFullPath(path);
        LayoutDocument result = new() {
            Name = Read(path, "layout", "name"),
            MasterOutput = Read(path, "layout", "master")
        };
        foreach (string name in Split(Read(path, "layout", "speakers"))) {
            string section = $"speaker.{name}";
            result.Speakers.Add(new SpeakerDefinition {
                Name = name,
                Azimuth = ReadDouble(path, section, "azimuth"),
                Elevation = ReadDouble(path, section, "elevation"),
                TrimDb = ReadDouble(path, section, "trim_db")
            });
        }
        foreach (string name in Split(Read(path, "layout", "outputs"))) {
            string section = $"output.{name}";
            OutputRouteDefinition output = new() {
                Name = name,
                Endpoint = Read(path, section, "endpoint"),
                DelayMilliseconds = ReadDouble(path, section, "delay_ms")
            };
            output.Speakers.AddRange(Split(Read(path, section, "speakers")));
            result.Outputs.Add(output);
        }
        if (result.Speakers.Count == 0 || result.Outputs.Count == 0) {
            throw new InvalidDataException("El perfil no contiene parlantes o salidas.");
        }
        return result;
    }

    public void Save(string path) {
        StringBuilder text = new();
        text.AppendLine("[layout]");
        text.AppendLine($"name={Name}");
        text.AppendLine($"speakers={string.Join(',', Speakers.Select(speaker => speaker.Name))}");
        text.AppendLine($"outputs={string.Join(',', Outputs.Select(output => output.Name))}");
        text.AppendLine($"master={MasterOutput}");
        foreach (SpeakerDefinition speaker in Speakers) {
            text.AppendLine();
            text.AppendLine($"[speaker.{speaker.Name}]");
            text.AppendLine($"azimuth={Number(speaker.Azimuth)}");
            text.AppendLine($"elevation={Number(speaker.Elevation)}");
            text.AppendLine($"trim_db={Number(speaker.TrimDb)}");
        }
        foreach (OutputRouteDefinition output in Outputs) {
            text.AppendLine();
            text.AppendLine($"[output.{output.Name}]");
            text.AppendLine($"endpoint={output.Endpoint}");
            text.AppendLine($"speakers={string.Join(',', output.Speakers)}");
            text.AppendLine($"delay_ms={Number(output.DelayMilliseconds)}");
        }
        File.WriteAllText(path, text.ToString().Replace("\r\n", "\n"),
            new UTF8Encoding(false));
    }

    public OutputRouteDefinition? OutputFor(string speakerName) =>
        Outputs.FirstOrDefault(output => output.Speakers.Contains(
            speakerName, StringComparer.OrdinalIgnoreCase));

    static string Read(string path, string section, string key) {
        StringBuilder value = new(BufferCharacters);
        uint copied = GetPrivateProfileString(
            section, key, "", value, BufferCharacters, path);
        if (copied == 0) throw new InvalidDataException($"Falta [{section}] {key}.");
        return value.ToString().Trim();
    }

    static double ReadDouble(string path, string section, string key) {
        string value = Read(path, section, key);
        if (!double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double result)) {
            throw new InvalidDataException($"Valor numerico invalido: [{section}] {key}.");
        }
        return result;
    }

    static IEnumerable<string> Split(string value) => value.Split(',')
        .Select(item => item.Trim())
        .Where(item => item.Length != 0);

    static string Number(double value) =>
        value.ToString("0.###", CultureInfo.InvariantCulture);
}
