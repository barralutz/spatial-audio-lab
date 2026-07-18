using System.Collections.ObjectModel;

namespace SpatialAudioLab.Core.Profiles;

public static class SpeakerCatalog
{
    private static readonly IReadOnlyDictionary<string, SpeakerDefinition> Speakers =
        new Dictionary<string, SpeakerDefinition>(StringComparer.OrdinalIgnoreCase)
        {
            ["FL"] = new("FL", -30, 0, 0),
            ["FR"] = new("FR", 30, 0, 0),
            ["FC"] = new("FC", 0, 0, 0),
            ["LFE"] = new("LFE", 0, 0, 0),
            ["BL"] = new("BL", -150, 0, 0),
            ["BR"] = new("BR", 150, 0, 0),
            ["SL"] = new("SL", -90, 0, 0),
            ["SR"] = new("SR", 90, 0, 0),
            ["TFL"] = new("TFL", -45, 45, 0),
            ["TFR"] = new("TFR", 45, 45, 0),
            ["TML"] = new("TML", -90, 90, 0),
            ["TMR"] = new("TMR", 90, 90, 0),
            ["TBL"] = new("TBL", -135, 45, 0),
            ["TBR"] = new("TBR", 135, 45, 0)
        };

    public static IReadOnlyList<SpeakerDefinition> Canonical714 { get; } =
        new ReadOnlyCollection<SpeakerDefinition>(
        [
            Get("FL"), Get("FR"), Get("FC"), Get("LFE"),
            Get("BL"), Get("BR"), Get("SL"), Get("SR"),
            Get("TFL"), Get("TFR"), Get("TBL"), Get("TBR")
        ]);

    internal static SpeakerDefinition Get(string name)
    {
        return Speakers.TryGetValue(name, out SpeakerDefinition? speaker)
            ? speaker
            : throw new KeyNotFoundException($"Unknown canonical speaker: {name}.");
    }
}
