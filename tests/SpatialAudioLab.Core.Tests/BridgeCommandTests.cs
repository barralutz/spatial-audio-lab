using SpatialAudioLab.Core.Runtime;
using Xunit;

namespace SpatialAudioLab.Core.Tests;

public sealed class BridgeCommandTests
{
    [Theory]
    [InlineData(BridgeMode.Atmos, "live-layout", "atmos")]
    [InlineData(BridgeMode.NativeMat, "live-layout", "native-mat")]
    [InlineData(BridgeMode.DtsX, "live-dtsx-layout", "dtsx")]
    [InlineData(BridgeMode.Pcm, "live-pcm-layout", "pcm")]
    public void Command_uses_portable_engine_profile_and_user_data_paths(
        BridgeMode mode,
        string engineCommand,
        string fileStem)
    {
        using TemporaryDirectory root = new();
        File.WriteAllText(Path.Combine(root.Path, "portable.flag"), string.Empty);
        RuntimePaths paths = RuntimePaths.Resolve(root.Path, root.Path);
        string profilePath = Path.Combine(paths.ProfilesRoot, "active.ini");

        BridgeCommand command = BridgeCommandFactory.Create(
            paths,
            mode,
            profilePath,
            0.25,
            40,
            BridgeLatencyMode.Balanced);

        Assert.Equal(paths.EngineExecutable, command.Executable);
        Assert.Equal(engineCommand, command.Arguments[0]);
        Assert.Equal("0", command.Arguments[1]);
        Assert.Equal(profilePath, command.Arguments[2]);
        Assert.Equal("0.25", command.Arguments[3]);
        Assert.Equal("40", command.Arguments[4]);
        Assert.Equal("balanced", command.Arguments[5]);
        Assert.Equal(Path.Combine(paths.LogsRoot, $"bridge-{fileStem}.log"), command.LogPath);
        Assert.Equal(Path.Combine(paths.LogsRoot, $"bridge-{fileStem}.err.log"), command.ErrorLogPath);
        Assert.Equal(Path.Combine(paths.LogsRoot, $"bridge-{fileStem}.pid"), command.PidPath);
        Assert.All(command.Arguments, argument =>
        {
            Assert.DoesNotContain("configs", argument, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("captures", argument, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("CMakeLists", argument, StringComparison.OrdinalIgnoreCase);
        });
    }

    [Fact]
    public void Invalid_bridge_controls_are_rejected()
    {
        using TemporaryDirectory root = new();
        RuntimePaths paths = RuntimePaths.Resolve(root.Path, root.Path);

        Assert.Throws<ArgumentOutOfRangeException>(() => BridgeCommandFactory.Create(
            paths,
            BridgeMode.Pcm,
            Path.Combine(root.Path, "profile.ini"),
            2,
            10,
            BridgeLatencyMode.Low));
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
