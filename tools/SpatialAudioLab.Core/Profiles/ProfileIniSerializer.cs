using System.Globalization;
using System.Text;

namespace SpatialAudioLab.Core.Profiles;

public static class ProfileIniSerializer
{
    private static readonly UTF8Encoding Utf8WithoutBom = new(false);

    public static ProfileDocument Load(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        IniDocument ini = IniDocument.Parse(File.ReadAllText(path, Encoding.UTF8));
        int version = ParseInt(ini.Required("profile", "version"), "profile version");
        if (version != ProfileDocument.CurrentVersion)
        {
            throw new InvalidDataException($"Unsupported profile version: {version}.");
        }

        ProfileDocument profile = ReadVersion2(ini, version);
        profile.Validate();
        return profile;
    }

    public static void Save(string path, ProfileDocument profile)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(profile);
        File.WriteAllText(path, Serialize(profile), Utf8WithoutBom);
    }

    public static ProfileDocument ImportVersion1(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        IniDocument ini = IniDocument.Parse(File.ReadAllText(path, Encoding.UTF8));
        List<string> speakerNames = SplitList(ini.Required("layout", "speakers"));
        List<string> outputNames = SplitList(ini.Required("layout", "outputs"));

        ProfileDocument profile = new()
        {
            Id = Guid.NewGuid(),
            Name = ini.Required("layout", "name"),
            LayoutId = InferLegacyLayoutId(speakerNames),
            MasterOutput = ini.Required("layout", "master"),
            Speakers = speakerNames.Select(name => ReadSpeaker(ini, name)).ToList(),
            Outputs = outputNames.Select(name => ReadVersion1Output(ini, name)).ToList()
        };
        profile.Validate();
        return profile;
    }

    internal static string Serialize(ProfileDocument profile)
    {
        profile.Validate();
        StringBuilder text = new();
        AppendSection(text, "profile");
        AppendValue(text, "version", profile.Version.ToString(CultureInfo.InvariantCulture));
        AppendValue(text, "id", profile.Id.ToString("D"));
        AppendValue(text, "name", profile.Name);
        AppendValue(text, "layout", profile.LayoutId);

        text.Append('\n');
        AppendSection(text, "layout");
        AppendValue(text, "speakers", string.Join(',', profile.Speakers.Select(s => s.Name)));
        AppendValue(text, "outputs", string.Join(',', profile.Outputs.Select(o => o.Name)));
        AppendValue(text, "master", profile.MasterOutput);

        foreach (SpeakerDefinition speaker in profile.Speakers)
        {
            text.Append('\n');
            AppendSection(text, $"speaker.{speaker.Name}");
            AppendValue(text, "azimuth", Number(speaker.Azimuth));
            AppendValue(text, "elevation", Number(speaker.Elevation));
            AppendValue(text, "trim_db", Number(speaker.TrimDb));
        }

        foreach (OutputRouteDefinition output in profile.Outputs)
        {
            text.Append('\n');
            AppendSection(text, $"output.{output.Name}");
            AppendValue(text, "endpoint_id", output.Endpoint.Id);
            AppendValue(text, "endpoint_name", output.Endpoint.FriendlyName);
            if (!string.IsNullOrWhiteSpace(output.Endpoint.ContainerId))
            {
                AppendValue(text, "container_id", output.Endpoint.ContainerId);
            }

            AppendValue(
                text,
                "expected_channels",
                output.Endpoint.ExpectedChannels.ToString(CultureInfo.InvariantCulture));
            AppendValue(text, "speakers", string.Join(',', output.Speakers));
            AppendValue(text, "delay_ms", Number(output.DelayMilliseconds));
        }

        return text.ToString();
    }

    private static ProfileDocument ReadVersion2(IniDocument ini, int version)
    {
        List<string> speakerNames = SplitList(ini.Required("layout", "speakers"));
        List<string> outputNames = SplitList(ini.Required("layout", "outputs"));
        if (!Guid.TryParse(ini.Required("profile", "id"), out Guid id))
        {
            throw new InvalidDataException("Profile ID is not a valid UUID.");
        }

        return new ProfileDocument
        {
            Version = version,
            Id = id,
            Name = ini.Required("profile", "name"),
            LayoutId = ini.Required("profile", "layout"),
            MasterOutput = ini.Required("layout", "master"),
            Speakers = speakerNames.Select(name => ReadSpeaker(ini, name)).ToList(),
            Outputs = outputNames.Select(name => ReadVersion2Output(ini, name)).ToList()
        };
    }

    private static SpeakerDefinition ReadSpeaker(IniDocument ini, string name)
    {
        string section = $"speaker.{name}";
        return new SpeakerDefinition(
            name,
            ParseDouble(ini.Required(section, "azimuth"), $"{name} azimuth"),
            ParseDouble(ini.Required(section, "elevation"), $"{name} elevation"),
            ParseDouble(ini.Optional(section, "trim_db") ?? "0", $"{name} trim"));
    }

    private static OutputRouteDefinition ReadVersion2Output(IniDocument ini, string name)
    {
        string section = $"output.{name}";
        return new OutputRouteDefinition(
            name,
            new EndpointIdentity(
                ini.Present(section, "endpoint_id"),
                ini.Required(section, "endpoint_name"),
                ini.Optional(section, "container_id"),
                ParseInt(
                    ini.Required(section, "expected_channels"),
                    $"{name} expected channels")),
            SplitList(ini.Required(section, "speakers")),
            ParseDouble(ini.Optional(section, "delay_ms") ?? "0", $"{name} delay"));
    }

    private static OutputRouteDefinition ReadVersion1Output(IniDocument ini, string name)
    {
        string section = $"output.{name}";
        List<string> speakers = SplitList(ini.Required(section, "speakers"));
        return new OutputRouteDefinition(
            name,
            new EndpointIdentity(
                string.Empty,
                ini.Required(section, "endpoint"),
                null,
                speakers.Count),
            speakers,
            ParseDouble(ini.Optional(section, "delay_ms") ?? "0", $"{name} delay"));
    }

    private static string InferLegacyLayoutId(IReadOnlyList<string> speakerNames)
    {
        string[] canonical714 =
        [
            "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR",
            "TFL", "TFR", "TBL", "TBR"
        ];
        return speakerNames.SequenceEqual(canonical714, StringComparer.OrdinalIgnoreCase)
            ? "7.1.4"
            : "custom";
    }

    private static List<string> SplitList(string value)
    {
        List<string> result = value.Split(',')
            .Select(item => item.Trim())
            .ToList();
        if (result.Count == 0 || result.Any(string.IsNullOrWhiteSpace))
        {
            throw new InvalidDataException("INI list contains an empty item.");
        }

        return result;
    }

    private static int ParseInt(string value, string description)
    {
        if (!int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out int result))
        {
            throw new InvalidDataException($"Invalid numeric value for {description}.");
        }

        return result;
    }

    private static double ParseDouble(string value, string description)
    {
        if (!double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double result) ||
            !double.IsFinite(result))
        {
            throw new InvalidDataException($"Invalid numeric value for {description}.");
        }

        return result;
    }

    private static string Number(double value) =>
        value.ToString("0.###############", CultureInfo.InvariantCulture);

    private static void AppendSection(StringBuilder text, string name) =>
        text.Append('[').Append(name).Append("]\n");

    private static void AppendValue(StringBuilder text, string key, string value) =>
        text.Append(key).Append('=').Append(value).Append('\n');

    private sealed class IniDocument
    {
        private readonly Dictionary<string, Dictionary<string, string>> sections =
            new(StringComparer.OrdinalIgnoreCase);

        public static IniDocument Parse(string text)
        {
            IniDocument result = new();
            Dictionary<string, string>? currentSection = null;
            string? currentSectionName = null;
            int lineNumber = 0;
            using StringReader reader = new(text);
            while (reader.ReadLine() is { } rawLine)
            {
                lineNumber++;
                string line = rawLine.Trim();
                if (line.Length == 0 || line.StartsWith(';') || line.StartsWith('#'))
                {
                    continue;
                }

                if (line.StartsWith('[') && line.EndsWith(']'))
                {
                    currentSectionName = line[1..^1].Trim();
                    if (currentSectionName.Length == 0 ||
                        !result.sections.TryAdd(
                            currentSectionName,
                            currentSection = new Dictionary<string, string>(
                                StringComparer.OrdinalIgnoreCase)))
                    {
                        throw new InvalidDataException(
                            $"Duplicate or empty INI section at line {lineNumber}.");
                    }

                    continue;
                }

                int separator = line.IndexOf('=');
                if (currentSection is null || separator <= 0)
                {
                    throw new InvalidDataException($"Malformed INI line {lineNumber}.");
                }

                string key = line[..separator].Trim();
                string value = line[(separator + 1)..].Trim();
                if (key.Length == 0 || !currentSection.TryAdd(key, value))
                {
                    throw new InvalidDataException(
                        $"Duplicate or empty INI key in [{currentSectionName}] at line {lineNumber}.");
                }
            }

            return result;
        }

        public string Required(string section, string key)
        {
            string value = Present(section, key);
            if (string.IsNullOrWhiteSpace(value))
            {
                throw new InvalidDataException($"Missing [{section}] {key}.");
            }

            return value;
        }

        public string Present(string section, string key)
        {
            if (!sections.TryGetValue(section, out Dictionary<string, string>? values) ||
                !values.TryGetValue(key, out string? value))
            {
                throw new InvalidDataException($"Missing [{section}] {key}.");
            }

            return value;
        }

        public string? Optional(string section, string key)
        {
            return sections.TryGetValue(section, out Dictionary<string, string>? values) &&
                   values.TryGetValue(key, out string? value) &&
                   !string.IsNullOrWhiteSpace(value)
                ? value
                : null;
        }
    }
}
