using System.Collections.ObjectModel;

namespace SpatialAudioLab.Core.Profiles;

public sealed record LayoutPreset(
    string Id,
    IReadOnlyList<SpeakerDefinition> Speakers);

public static class LayoutPresetCatalog
{
    private static readonly IReadOnlyDictionary<string, LayoutPreset> Presets =
        CreatePresets();

    public static IReadOnlyList<LayoutPreset> All { get; } =
        new ReadOnlyCollection<LayoutPreset>(Presets.Values.ToList());

    public static LayoutPreset Get(string id)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(id);
        return Presets.TryGetValue(id, out LayoutPreset? preset)
            ? preset
            : throw new KeyNotFoundException($"Unknown layout preset: {id}.");
    }

    public static LayoutPreset CreateCustom(
        string id,
        IEnumerable<SpeakerDefinition> speakers)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(id);
        ArgumentNullException.ThrowIfNull(speakers);
        List<SpeakerDefinition> selected = speakers.ToList();
        if (selected.Count is < 2 or > 12)
        {
            throw new InvalidDataException("A custom layout must contain between 2 and 12 speakers.");
        }

        HashSet<string> names = new(StringComparer.OrdinalIgnoreCase);
        foreach (SpeakerDefinition speaker in selected)
        {
            if (string.IsNullOrWhiteSpace(speaker.Name) || !names.Add(speaker.Name))
            {
                throw new InvalidDataException("Custom speaker names must be non-empty and unique.");
            }

            if (!double.IsFinite(speaker.Azimuth) ||
                !double.IsFinite(speaker.Elevation) ||
                !double.IsFinite(speaker.TrimDb) ||
                speaker.Elevation is < -90 or > 90 ||
                speaker.TrimDb is < -60 or > 12)
            {
                throw new InvalidDataException("Custom speaker geometry or trim is invalid.");
            }
        }

        if (!names.Contains("FL") || !names.Contains("FR"))
        {
            throw new InvalidDataException("A custom layout requires FL and FR speakers.");
        }

        int lfeCount = selected.Count(speaker =>
            speaker.Name.Equals("LFE", StringComparison.OrdinalIgnoreCase));
        int heightCount = selected.Count(speaker => speaker.Elevation >= 25);
        int earLevelCount = selected.Count(speaker =>
            speaker.Elevation < 25 &&
            !speaker.Name.Equals("LFE", StringComparison.OrdinalIgnoreCase));
        if (lfeCount > 1 || heightCount > 4 || earLevelCount > 7)
        {
            throw new InvalidDataException(
                "A custom layout exceeds the supported bed, LFE, or height limits.");
        }

        return new LayoutPreset(
            id,
            new ReadOnlyCollection<SpeakerDefinition>(selected));
    }

    private static IReadOnlyDictionary<string, LayoutPreset> CreatePresets()
    {
        LayoutPreset[] presets =
        [
            Preset("2.0", "FL", "FR"),
            Preset("2.1", "FL", "FR", "LFE"),
            Preset("3.1", "FL", "FR", "FC", "LFE"),
            Preset("4.0", "FL", "FR", "SL", "SR"),
            Preset("4.1", "FL", "FR", "LFE", "SL", "SR"),
            Preset("5.1", "FL", "FR", "FC", "LFE", "SL", "SR"),
            Preset("7.1", "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"),
            Preset(
                "5.1.2-top-front",
                "FL", "FR", "FC", "LFE", "SL", "SR", "TFL", "TFR"),
            Preset(
                "5.1.2-top-middle",
                "FL", "FR", "FC", "LFE", "SL", "SR", "TML", "TMR"),
            Preset(
                "5.1.2-top-rear",
                "FL", "FR", "FC", "LFE", "SL", "SR", "TBL", "TBR"),
            Preset(
                "5.1.4",
                "FL", "FR", "FC", "LFE", "SL", "SR",
                "TFL", "TFR", "TBL", "TBR"),
            Preset(
                "7.1.2-top-front",
                "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR", "TFL", "TFR"),
            Preset(
                "7.1.2-top-middle",
                "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR", "TML", "TMR"),
            Preset(
                "7.1.2-top-rear",
                "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR", "TBL", "TBR"),
            Preset(
                "7.1.4",
                "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR",
                "TFL", "TFR", "TBL", "TBR")
        ];
        return new ReadOnlyDictionary<string, LayoutPreset>(
            presets.ToDictionary(preset => preset.Id, StringComparer.OrdinalIgnoreCase));
    }

    private static LayoutPreset Preset(string id, params string[] speakerNames)
    {
        return new LayoutPreset(
            id,
            new ReadOnlyCollection<SpeakerDefinition>(
                speakerNames.Select(SpeakerCatalog.Get).ToList()));
    }
}
