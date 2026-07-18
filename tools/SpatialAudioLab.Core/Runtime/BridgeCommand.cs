using System.Globalization;

namespace SpatialAudioLab.Core.Runtime;

public enum BridgeMode
{
    Atmos,
    NativeMat,
    DtsX,
    Pcm
}

public enum BridgeLatencyMode
{
    Safe,
    Balanced,
    Low
}

public sealed record BridgeCommand(
    string Executable,
    IReadOnlyList<string> Arguments,
    string LogPath,
    string ErrorLogPath,
    string PidPath);

public static class BridgeCommandFactory
{
    public static BridgeCommand Create(
        RuntimePaths paths,
        BridgeMode mode,
        string profilePath,
        double gain,
        int prebufferMilliseconds,
        BridgeLatencyMode latencyMode)
    {
        ArgumentNullException.ThrowIfNull(paths);
        ArgumentException.ThrowIfNullOrWhiteSpace(profilePath);
        if (!double.IsFinite(gain) || gain is < 0 or > 1)
        {
            throw new ArgumentOutOfRangeException(nameof(gain));
        }

        if (prebufferMilliseconds is < 20 or > 500)
        {
            throw new ArgumentOutOfRangeException(nameof(prebufferMilliseconds));
        }

        (string engineCommand, string fileStem) = mode switch
        {
            BridgeMode.Atmos => ("live-layout", "atmos"),
            BridgeMode.NativeMat => ("live-layout", "native-mat"),
            BridgeMode.DtsX => ("live-dtsx-layout", "dtsx"),
            BridgeMode.Pcm => ("live-pcm-layout", "pcm"),
            _ => throw new ArgumentOutOfRangeException(nameof(mode))
        };

        string normalizedProfilePath = Path.GetFullPath(profilePath);
        string latency = latencyMode.ToString().ToLowerInvariant();
        string[] arguments =
        [
            engineCommand,
            "0",
            normalizedProfilePath,
            gain.ToString("0.###", CultureInfo.InvariantCulture),
            prebufferMilliseconds.ToString(CultureInfo.InvariantCulture),
            latency
        ];

        return new BridgeCommand(
            paths.EngineExecutable,
            arguments,
            Path.Combine(paths.LogsRoot, $"bridge-{fileStem}.log"),
            Path.Combine(paths.LogsRoot, $"bridge-{fileStem}.err.log"),
            Path.Combine(paths.LogsRoot, $"bridge-{fileStem}.pid"));
    }
}
