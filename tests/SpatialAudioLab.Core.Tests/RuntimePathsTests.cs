using SpatialAudioLab.Core.Runtime;
using Xunit;

namespace SpatialAudioLab.Core.Tests;

public sealed class RuntimePathsTests
{
    [Fact]
    public void Portable_flag_keeps_mutable_data_beside_application()
    {
        using TemporaryDirectory root = new();
        File.WriteAllText(Path.Combine(root.Path, "portable.flag"), string.Empty);

        RuntimePaths paths = RuntimePaths.Resolve(
            root.Path,
            @"C:\Users\Test\AppData\Local");

        Assert.True(paths.IsPortable);
        Assert.Equal(Path.Combine(root.Path, "Data"), paths.DataRoot);
        Assert.Equal(
            Path.Combine(root.Path, "Engine", "SpatialAudioLab.CLI.exe"),
            paths.EngineExecutable);
    }

    [Fact]
    public void Installed_mode_uses_local_app_data()
    {
        using TemporaryDirectory root = new();

        RuntimePaths paths = RuntimePaths.Resolve(
            root.Path,
            @"C:\Users\Test\AppData\Local");

        Assert.False(paths.IsPortable);
        Assert.Equal(
            @"C:\Users\Test\AppData\Local\SpatialAudioLab",
            paths.DataRoot);
    }

    [Fact]
    public void Resolver_exposes_tools_below_application_root()
    {
        using TemporaryDirectory root = new();

        RuntimePaths paths = RuntimePaths.Resolve(
            root.Path,
            @"C:\Users\Test\AppData\Local");

        Assert.Equal(
            Path.Combine(root.Path, "Tools", "ffmpeg", "bin", "ffprobe.exe"),
            paths.FfprobeExecutable);
        Assert.Equal(
            Path.Combine(root.Path, "Tools", "mpv", "mpv.exe"),
            paths.MpvExecutable);
        Assert.Equal(
            Path.Combine(root.Path, "Tools", "truehdd", "truehd-stream.exe"),
            paths.TrueHdExecutable);
    }

    [Fact]
    public void Ensure_user_directories_creates_only_mutable_directories()
    {
        using TemporaryDirectory root = new();
        RuntimePaths paths = RuntimePaths.Resolve(root.Path, root.Path);

        paths.EnsureUserDirectories();

        Assert.True(Directory.Exists(paths.DataRoot));
        Assert.True(Directory.Exists(paths.ProfilesRoot));
        Assert.True(Directory.Exists(paths.LogsRoot));
        Assert.True(Directory.Exists(paths.CacheRoot));
        Assert.False(Directory.Exists(Path.Combine(root.Path, "Engine")));
        Assert.False(Directory.Exists(Path.Combine(root.Path, "Tools")));
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

        public void Dispose()
        {
            Directory.Delete(Path, recursive: true);
        }
    }
}
