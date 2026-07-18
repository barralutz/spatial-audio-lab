using System.Text;
using SpatialAudioLab.Core.Profiles;
using Xunit;

namespace SpatialAudioLab.Core.Tests;

public sealed class ProfileRepositoryTests
{
    [Fact]
    public void Save_and_set_active_round_trip_profile_without_temporary_files()
    {
        using TemporaryDirectory root = new();
        ProfileRepository repository = new(root.Path);
        ProfileDocument profile = CreateStereoProfile();

        repository.Save(profile);
        repository.SetActive(profile.Id);
        ProfileDocument? loaded = repository.LoadActive();

        Assert.NotNull(loaded);
        Assert.Equal(profile.Id, loaded.Id);
        Assert.Equal(profile.Name, loaded.Name);
        Assert.Equal(profile.Id.ToString("D"), File.ReadAllText(
            Path.Combine(root.Path, "active-profile.txt"), Encoding.UTF8).Trim());
        Assert.Empty(Directory.EnumerateFiles(root.Path, "*.tmp"));
    }

    [Fact]
    public void List_reports_corrupt_profile_without_deleting_it()
    {
        using TemporaryDirectory root = new();
        ProfileRepository repository = new(root.Path);
        Guid id = Guid.Parse("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
        string path = Path.Combine(root.Path, $"{id:D}.ini");
        File.WriteAllText(path, "[profile]\nversion=2\n", new UTF8Encoding(false));

        ProfileListEntry entry = Assert.Single(repository.List());

        Assert.Equal(path, entry.Path);
        Assert.Null(entry.Profile);
        Assert.NotNull(entry.Error);
        Assert.True(File.Exists(path));
    }

    [Fact]
    public void Load_active_excludes_corrupt_profile()
    {
        using TemporaryDirectory root = new();
        ProfileRepository repository = new(root.Path);
        ProfileDocument profile = CreateStereoProfile();
        repository.Save(profile);
        repository.SetActive(profile.Id);
        File.WriteAllText(
            Path.Combine(root.Path, $"{profile.Id:D}.ini"),
            "corrupt",
            new UTF8Encoding(false));

        ProfileDocument? loaded = repository.LoadActive();

        Assert.Null(loaded);
    }

    [Fact]
    public void Delete_removes_profile_and_its_active_marker()
    {
        using TemporaryDirectory root = new();
        ProfileRepository repository = new(root.Path);
        ProfileDocument profile = CreateStereoProfile();
        repository.Save(profile);
        repository.SetActive(profile.Id);

        repository.Delete(profile.Id);

        Assert.Empty(repository.List());
        Assert.False(File.Exists(Path.Combine(root.Path, "active-profile.txt")));
    }

    private static ProfileDocument CreateStereoProfile()
    {
        return new ProfileDocument
        {
            Id = Guid.Parse("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"),
            Name = "Desktop stereo",
            LayoutId = "2.0",
            MasterOutput = "main",
            Speakers =
            [
                new SpeakerDefinition("FL", -30, 0, 0),
                new SpeakerDefinition("FR", 30, 0, 0)
            ],
            Outputs =
            [
                new OutputRouteDefinition(
                    "main",
                    new EndpointIdentity("endpoint-1", "Speakers", null, 2),
                    ["FL", "FR"],
                    0)
            ]
        };
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
}
