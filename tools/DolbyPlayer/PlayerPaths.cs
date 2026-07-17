namespace DolbyPlayer;

internal sealed class PlayerPaths {
    public string RepoRoot { get; }
    public string TrueHdStream { get; }
    public string Mpv { get; }
    public string Wsl { get; }
    public string CacheRoot { get; }

    public PlayerPaths() {
        DirectoryInfo? current = new(AppContext.BaseDirectory);
        while (current != null && !File.Exists(Path.Combine(current.FullName, "README.md"))) {
            current = current.Parent;
        }
        RepoRoot = current?.FullName ?? throw new DirectoryNotFoundException(
            "Could not locate the SpatialAudioLab repository root.");
        TrueHdStream = Path.Combine(RepoRoot, "tools", "truehdd", "truehd-stream.exe");
        Mpv = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            "MPV Player", "mpv.exe");
        Wsl = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),
            "System32", "wsl.exe");
        CacheRoot = Path.Combine(RepoRoot, "captures", "player-cache");
    }

    public void Validate(bool requireMpv) {
        if (!File.Exists(TrueHdStream)) throw new FileNotFoundException(
            "The streaming TrueHD decoder is not built.", TrueHdStream);
        if (!File.Exists(Wsl)) throw new FileNotFoundException("WSL is required for FFmpeg.", Wsl);
        if (requireMpv && !File.Exists(Mpv)) throw new FileNotFoundException("mpv is not installed.", Mpv);
        Directory.CreateDirectory(CacheRoot);
    }

    public static string ToWslPath(string windowsPath) {
        string fullPath = Path.GetFullPath(windowsPath);
        if (fullPath.Length < 3 || fullPath[1] != ':' || fullPath[2] != '\\') {
            throw new NotSupportedException($"Only local Windows paths can be passed to WSL: {fullPath}");
        }
        char drive = char.ToLowerInvariant(fullPath[0]);
        return $"/mnt/{drive}/{fullPath[3..].Replace('\\', '/')}";
    }
}
