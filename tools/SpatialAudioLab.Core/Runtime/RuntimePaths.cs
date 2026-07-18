namespace SpatialAudioLab.Core.Runtime;

public sealed record RuntimePaths(
    string ApplicationRoot,
    string DataRoot,
    string ProfilesRoot,
    string LogsRoot,
    string CacheRoot,
    string EngineExecutable,
    string FfprobeExecutable,
    string MpvExecutable,
    string TrueHdExecutable,
    bool IsPortable)
{
    public static RuntimePaths Resolve(
        string applicationRoot,
        string localAppDataRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(applicationRoot);
        ArgumentException.ThrowIfNullOrWhiteSpace(localAppDataRoot);

        string normalizedApplicationRoot = Path.GetFullPath(applicationRoot);
        bool isPortable = File.Exists(
            Path.Combine(normalizedApplicationRoot, "portable.flag"));
        string dataRoot = Path.GetFullPath(
            isPortable
                ? Path.Combine(normalizedApplicationRoot, "Data")
                : Path.Combine(localAppDataRoot, "SpatialAudioLab"));

        return new RuntimePaths(
            normalizedApplicationRoot,
            dataRoot,
            Path.Combine(dataRoot, "Profiles"),
            Path.Combine(dataRoot, "Logs"),
            Path.Combine(dataRoot, "Cache"),
            Path.Combine(
                normalizedApplicationRoot,
                "Engine",
                "SpatialAudioLab.CLI.exe"),
            Path.Combine(
                normalizedApplicationRoot,
                "Tools",
                "ffmpeg",
                "bin",
                "ffprobe.exe"),
            Path.Combine(
                normalizedApplicationRoot,
                "Tools",
                "mpv",
                "mpv.exe"),
            Path.Combine(
                normalizedApplicationRoot,
                "Tools",
                "truehdd",
                "truehd-stream.exe"),
            isPortable);
    }

    public static RuntimePaths ResolveForCurrentProcess()
    {
        return Resolve(
            AppContext.BaseDirectory,
            Environment.GetFolderPath(
                Environment.SpecialFolder.LocalApplicationData));
    }

    public void EnsureUserDirectories()
    {
        Directory.CreateDirectory(DataRoot);
        Directory.CreateDirectory(ProfilesRoot);
        Directory.CreateDirectory(LogsRoot);
        Directory.CreateDirectory(CacheRoot);
    }
}
