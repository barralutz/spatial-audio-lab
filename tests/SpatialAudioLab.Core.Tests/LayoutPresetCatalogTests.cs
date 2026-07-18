using SpatialAudioLab.Core.Profiles;
using Xunit;

namespace SpatialAudioLab.Core.Tests;

public sealed class LayoutPresetCatalogTests
{
    [Fact]
    public void Catalog_contains_supported_presets_with_unique_ids()
    {
        string[] requiredIds =
        [
            "2.0", "2.1", "3.1", "4.0", "4.1", "5.1", "7.1",
            "5.1.2-top-front", "5.1.2-top-middle", "5.1.2-top-rear",
            "5.1.4", "7.1.2-top-front", "7.1.2-top-middle",
            "7.1.2-top-rear", "7.1.4"
        ];

        Assert.Equal(
            LayoutPresetCatalog.All.Count,
            LayoutPresetCatalog.All.Select(preset => preset.Id)
                .Distinct(StringComparer.OrdinalIgnoreCase).Count());
        Assert.All(requiredIds, id => Assert.Contains(
            LayoutPresetCatalog.All,
            preset => preset.Id.Equals(id, StringComparison.OrdinalIgnoreCase)));
    }

    [Fact]
    public void Seven_one_four_uses_canonical_mat_order()
    {
        string[] expected =
        [
            "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR",
            "TFL", "TFR", "TBL", "TBR"
        ];

        LayoutPreset preset = LayoutPresetCatalog.Get("7.1.4");

        Assert.Equal(expected, preset.Speakers.Select(speaker => speaker.Name));
        Assert.Equal(expected, SpeakerCatalog.Canonical714.Select(speaker => speaker.Name));
        Assert.All(preset.Speakers, speaker => Assert.Equal(0, speaker.TrimDb));
    }

    [Theory]
    [InlineData("5.1.2-top-front", "TFL", "TFR")]
    [InlineData("5.1.2-top-middle", "TML", "TMR")]
    [InlineData("5.1.2-top-rear", "TBL", "TBR")]
    [InlineData("7.1.2-top-front", "TFL", "TFR")]
    [InlineData("7.1.2-top-middle", "TML", "TMR")]
    [InlineData("7.1.2-top-rear", "TBL", "TBR")]
    public void Height_pair_presets_use_the_named_overhead_position(
        string id,
        string left,
        string right)
    {
        LayoutPreset preset = LayoutPresetCatalog.Get(id);

        Assert.Contains(preset.Speakers, speaker => speaker.Name == left);
        Assert.Contains(preset.Speakers, speaker => speaker.Name == right);
        Assert.Equal(2, preset.Speakers.Count(speaker => speaker.Elevation >= 25));
    }

    [Fact]
    public void Custom_three_one_two_is_accepted()
    {
        SpeakerDefinition[] speakers =
        [
            new("FL", -30, 0, 0),
            new("FR", 30, 0, 0),
            new("FC", 0, 0, 0),
            new("LFE", 0, 0, 0),
            new("TFL", -45, 45, 0),
            new("TFR", 45, 45, 0)
        ];

        LayoutPreset custom = LayoutPresetCatalog.CreateCustom("3.1.2", speakers);

        Assert.Equal("3.1.2", custom.Id);
        Assert.Equal(speakers, custom.Speakers);
    }

    [Fact]
    public void Custom_layout_rejects_more_than_twelve_speakers()
    {
        List<SpeakerDefinition> speakers =
        [
            new("FL", -30, 0, 0),
            new("FR", 30, 0, 0)
        ];
        speakers.AddRange(Enumerable.Range(1, 11).Select(index =>
            new SpeakerDefinition($"X{index}", index, 0, 0)));

        Assert.Throws<InvalidDataException>(
            () => LayoutPresetCatalog.CreateCustom("too-many", speakers));
    }

    [Fact]
    public void Custom_layout_rejects_five_height_speakers()
    {
        SpeakerDefinition[] speakers =
        [
            new("FL", -30, 0, 0),
            new("FR", 30, 0, 0),
            new("H1", -60, 45, 0),
            new("H2", -30, 45, 0),
            new("H3", 0, 45, 0),
            new("H4", 30, 45, 0),
            new("H5", 60, 45, 0)
        ];

        Assert.Throws<InvalidDataException>(
            () => LayoutPresetCatalog.CreateCustom("too-high", speakers));
    }

    [Fact]
    public void Custom_layout_rejects_duplicate_names()
    {
        SpeakerDefinition[] speakers =
        [
            new("FL", -30, 0, 0),
            new("FR", 30, 0, 0),
            new("fl", -20, 0, 0)
        ];

        Assert.Throws<InvalidDataException>(
            () => LayoutPresetCatalog.CreateCustom("duplicate", speakers));
    }

    [Fact]
    public void Custom_layout_rejects_a_single_speaker()
    {
        SpeakerDefinition[] speakers = [new("FL", -30, 0, 0)];

        Assert.Throws<InvalidDataException>(
            () => LayoutPresetCatalog.CreateCustom("mono", speakers));
    }
}
