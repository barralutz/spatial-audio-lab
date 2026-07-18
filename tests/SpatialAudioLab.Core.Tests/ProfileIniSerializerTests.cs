using System.Text;
using SpatialAudioLab.Core.Profiles;
using Xunit;

namespace SpatialAudioLab.Core.Tests;

public sealed class ProfileIniSerializerTests
{
    [Fact]
    public void Version_2_profile_round_trips_without_losing_routing_data()
    {
        using TemporaryDirectory root = new();
        string inputPath = Path.Combine(root.Path, "input.ini");
        string outputPath = Path.Combine(root.Path, "output.ini");
        File.WriteAllText(inputPath, Version2Fixture, new UTF8Encoding(false));

        ProfileDocument loaded = ProfileIniSerializer.Load(inputPath);
        ProfileIniSerializer.Save(outputPath, loaded);
        ProfileDocument roundTripped = ProfileIniSerializer.Load(outputPath);

        Assert.Equal(ProfileDocument.CurrentVersion, roundTripped.Version);
        Assert.Equal(Guid.Parse("11111111-2222-3333-4444-555555555555"), roundTripped.Id);
        Assert.Equal("Living room 5.1.2", roundTripped.Name);
        Assert.Equal("5.1.2-top-middle", roundTripped.LayoutId);
        Assert.Equal("main", roundTripped.MasterOutput);
        Assert.Equal(
            ["FL", "FR", "FC", "LFE", "SL", "SR", "TML", "TMR"],
            roundTripped.Speakers.Select(speaker => speaker.Name));
        Assert.Equal(-30, roundTripped.Speakers[0].Azimuth);
        Assert.Equal(45, roundTripped.Speakers[6].Elevation);
        Assert.Equal(-1.5, roundTripped.Speakers[6].TrimDb);

        OutputRouteDefinition main = roundTripped.Outputs[0];
        Assert.Equal("main", main.Name);
        Assert.Equal(
            "{0.0.0.00000000}.{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}",
            main.Endpoint.Id);
        Assert.Equal("Speakers", main.Endpoint.FriendlyName);
        Assert.Equal("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", main.Endpoint.ContainerId);
        Assert.Equal(6, main.Endpoint.ExpectedChannels);
        Assert.Equal(["FL", "FR", "FC", "LFE", "SL", "SR"], main.Speakers);
        Assert.Equal(0, main.DelayMilliseconds);
        Assert.Equal(12.5, roundTripped.Outputs[1].DelayMilliseconds);
    }

    [Fact]
    public void Save_emits_utf8_without_bom_and_lf_line_endings()
    {
        using TemporaryDirectory root = new();
        string inputPath = Path.Combine(root.Path, "input.ini");
        string outputPath = Path.Combine(root.Path, "output.ini");
        File.WriteAllText(inputPath, Version2Fixture, new UTF8Encoding(false));

        ProfileIniSerializer.Save(outputPath, ProfileIniSerializer.Load(inputPath));

        byte[] bytes = File.ReadAllBytes(outputPath);
        Assert.False(bytes.AsSpan().StartsWith(Encoding.UTF8.Preamble));
        Assert.DoesNotContain('\r', Encoding.UTF8.GetString(bytes));
    }

    [Fact]
    public void Import_version_1_preserves_source_and_creates_migratable_profile()
    {
        string sourcePath = FindRepositoryFile("configs", "realtek-c1u-714.ini");
        byte[] before = File.ReadAllBytes(sourcePath);

        ProfileDocument imported = ProfileIniSerializer.ImportVersion1(sourcePath);

        Assert.Equal(before, File.ReadAllBytes(sourcePath));
        Assert.Equal(ProfileDocument.CurrentVersion, imported.Version);
        Assert.NotEqual(Guid.Empty, imported.Id);
        Assert.Equal("Realtek + front panel + C-1U 7.1.4", imported.Name);
        Assert.Equal("7.1.4", imported.LayoutId);
        Assert.Equal(12, imported.Speakers.Count);
        Assert.Equal([8, 2, 2], imported.Outputs.Select(output => output.Endpoint.ExpectedChannels));
        Assert.All(imported.Outputs, output => Assert.Equal(string.Empty, output.Endpoint.Id));
        Assert.Equal("Altavoces (Realtek(R) Audio)", imported.Outputs[0].Endpoint.FriendlyName);
    }

    [Fact]
    public void Load_rejects_duplicate_section_keys()
    {
        using TemporaryDirectory root = new();
        string path = Path.Combine(root.Path, "duplicate.ini");
        File.WriteAllText(
            path,
            Version2Fixture.Replace(
                "name=Living room 5.1.2",
                "name=Living room 5.1.2\nname=Duplicate"),
            new UTF8Encoding(false));

        InvalidDataException error = Assert.Throws<InvalidDataException>(
            () => ProfileIniSerializer.Load(path));

        Assert.Contains("duplicate", error.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Validation_rejects_a_speaker_assigned_more_than_once()
    {
        using TemporaryDirectory root = new();
        string path = Path.Combine(root.Path, "input.ini");
        File.WriteAllText(path, Version2Fixture, new UTF8Encoding(false));
        ProfileDocument profile = ProfileIniSerializer.Load(path);
        profile.Outputs[1] = profile.Outputs[1] with
        {
            Speakers = ["FL", "TMR"]
        };

        InvalidDataException error = Assert.Throws<InvalidDataException>(profile.Validate);

        Assert.Contains("exactly one", error.Message, StringComparison.OrdinalIgnoreCase);
    }

    private static string FindRepositoryFile(params string[] parts)
    {
        DirectoryInfo? directory = new(AppContext.BaseDirectory);
        while (directory is not null)
        {
            string candidate = Path.Combine([directory.FullName, .. parts]);
            if (File.Exists(candidate))
            {
                return candidate;
            }

            directory = directory.Parent;
        }

        throw new FileNotFoundException($"Repository fixture not found: {Path.Combine(parts)}");
    }

    private sealed class TemporaryDirectory : IDisposable
    {
        public TemporaryDirectory()
        {
            Path = System.IO.Path.Combine(
                System.IO.Path.GetTempPath(),
                $"SpatialAudioLab-{Guid.NewGuid():N}");
            Directory.CreateDirectory(Path);
        }

        public string Path { get; }

        public void Dispose() => Directory.Delete(Path, recursive: true);
    }

    private const string Version2Fixture = """
        [profile]
        version=2
        id=11111111-2222-3333-4444-555555555555
        name=Living room 5.1.2
        layout=5.1.2-top-middle

        [layout]
        speakers=FL,FR,FC,LFE,SL,SR,TML,TMR
        outputs=main,height
        master=main

        [speaker.FL]
        azimuth=-30
        elevation=0
        trim_db=0

        [speaker.FR]
        azimuth=30
        elevation=0
        trim_db=0

        [speaker.FC]
        azimuth=0
        elevation=0
        trim_db=0

        [speaker.LFE]
        azimuth=0
        elevation=0
        trim_db=0

        [speaker.SL]
        azimuth=-90
        elevation=0
        trim_db=0

        [speaker.SR]
        azimuth=90
        elevation=0
        trim_db=0

        [speaker.TML]
        azimuth=-90
        elevation=45
        trim_db=-1.5

        [speaker.TMR]
        azimuth=90
        elevation=45
        trim_db=1.25

        [output.main]
        endpoint_id={0.0.0.00000000}.{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}
        endpoint_name=Speakers
        container_id=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee
        expected_channels=6
        speakers=FL,FR,FC,LFE,SL,SR
        delay_ms=0

        [output.height]
        endpoint_id={0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}
        endpoint_name=USB Speakers
        expected_channels=2
        speakers=TML,TMR
        delay_ms=12.5
        """;
}
