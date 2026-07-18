using DolbyPlayer;
using SpatialAudioLab.Core.Profiles;
using SpatialAudioLab.Core.Runtime;
using Xunit;

namespace SpatialAudioLab.Cinema.Tests;

public sealed class PlayerPathsTests
{
    [Fact]
    public void Tools_resolve_below_application_root_and_cache_below_data_root()
    {
        using TemporaryDirectory root = new();
        File.WriteAllText(Path.Combine(root.Path, "portable.flag"), string.Empty);
        RuntimePaths runtime = RuntimePaths.Resolve(root.Path, root.Path);

        PlayerPaths paths = new(runtime);

        Assert.Equal(runtime.FfprobeExecutable, paths.Ffprobe);
        Assert.Equal(runtime.MpvExecutable, paths.Mpv);
        Assert.Equal(runtime.TrueHdExecutable, paths.TrueHdStream);
        Assert.StartsWith(runtime.CacheRoot, paths.CacheRoot, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("captures", paths.CacheRoot, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Virtual_sink_filter_comes_from_setup_owned_product_settings()
    {
        using TemporaryDirectory root = new();
        File.WriteAllText(Path.Combine(root.Path, "portable.flag"), string.Empty);
        RuntimePaths runtime = RuntimePaths.Resolve(root.Path, root.Path);
        Directory.CreateDirectory(runtime.DataRoot);
        File.WriteAllText(
            Path.Combine(runtime.DataRoot, "product-settings.json"),
            """{"virtualSinkEndpointId":"endpoint-spatial"}""");

        PlayerPaths paths = new(runtime);

        Assert.Equal("endpoint-spatial", paths.VirtualSinkFilter);
    }

    [Fact]
    public void Virtual_sink_filter_has_a_product_name_fallback()
    {
        using TemporaryDirectory root = new();
        RuntimePaths runtime = RuntimePaths.Resolve(root.Path, root.Path);

        PlayerPaths paths = new(runtime);

        Assert.Equal("SpatialAudioLab Virtual Sink", paths.VirtualSinkFilter);
        Assert.DoesNotContain("HISENSE", paths.VirtualSinkFilter, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Active_profile_is_loaded_from_the_runtime_profile_repository()
    {
        using TemporaryDirectory root = new();
        RuntimePaths runtime = RuntimePaths.Resolve(root.Path, root.Path);
        ProfileRepository repository = new(runtime.ProfilesRoot);
        ProfileDocument expected = StereoProfile();
        repository.Save(expected);
        repository.SetActive(expected.Id);

        PlayerPaths paths = new(runtime);
        ProfileDocument profile = paths.RequireActiveProfile();

        Assert.Equal(expected.Id, profile.Id);
        Assert.Equal("2.0", profile.LayoutId);
    }

    [Fact]
    public void Missing_active_profile_reports_that_studio_setup_is_required()
    {
        using TemporaryDirectory root = new();
        PlayerPaths paths = new(RuntimePaths.Resolve(root.Path, root.Path));

        InvalidOperationException error = Assert.Throws<InvalidOperationException>(
            paths.RequireActiveProfile);

        Assert.Contains("Studio", error.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Diagnostic_sink_override_wins_over_product_settings()
    {
        using TemporaryDirectory root = new();
        PlayerPaths paths = new(RuntimePaths.Resolve(root.Path, root.Path));

        Assert.Equal("diagnostic-endpoint", paths.ResolveVirtualSinkFilter("diagnostic-endpoint"));
        Assert.Equal(paths.VirtualSinkFilter, paths.ResolveVirtualSinkFilter(null));
    }

    private static ProfileDocument StereoProfile()
    {
        return new ProfileDocument
        {
            Id = Guid.Parse("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"),
            Name = "Cinema stereo",
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
                    new EndpointIdentity("endpoint", "Speakers", null, 2),
                    ["FL", "FR"],
                    0)
            ]
        };
    }
}
