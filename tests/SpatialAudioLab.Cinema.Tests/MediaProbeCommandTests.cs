using DolbyPlayer;
using SpatialAudioLab.Core.Runtime;
using Xunit;

namespace SpatialAudioLab.Cinema.Tests;

public sealed class MediaProbeCommandTests
{
    [Fact]
    public void Ffprobe_receives_the_literal_windows_media_path()
    {
        using TemporaryDirectory root = new();
        RuntimePaths runtime = RuntimePaths.Resolve(root.Path, root.Path);
        PlayerPaths paths = new(runtime);
        const string mediaPath = @"D:\Films\Atmos Movie.mkv";

        MediaProbeCommand command = MediaProbe.CreateCommand(paths, mediaPath);

        Assert.Equal(runtime.FfprobeExecutable, command.Executable);
        Assert.Equal(mediaPath, command.Arguments[^1]);
        Assert.DoesNotContain(command.Arguments, argument =>
            argument.Contains("/mnt/", StringComparison.OrdinalIgnoreCase));
        Assert.DoesNotContain(command.Arguments, argument =>
            argument.Contains("wsl", StringComparison.OrdinalIgnoreCase));
    }
}
