using System.Text.Json;

namespace DolbyPlayer;

internal enum AtmosCodec { Eac3Joc, TrueHdAtmos }

internal sealed record AudioStreamInfo(int Index, AtmosCodec Codec, int Channels, int SampleRate,
                                       string Language, string Title);

internal sealed record MediaInfo(double DurationSeconds, IReadOnlyList<AudioStreamInfo> AtmosStreams) {
    public AudioStreamInfo Select(int? streamIndex) {
        AudioStreamInfo? result = streamIndex.HasValue
            ? AtmosStreams.FirstOrDefault(stream => stream.Index == streamIndex.Value)
            : AtmosStreams.FirstOrDefault();
        return result ?? throw new InvalidOperationException(streamIndex.HasValue
            ? $"Stream 0:{streamIndex} is not E-AC-3 JOC or TrueHD Atmos."
            : "No E-AC-3 JOC or TrueHD Atmos stream was found.");
    }
}

internal static class MediaProbe {
    public static async Task<MediaInfo> ReadAsync(PlayerPaths paths, string input,
                                                  CancellationToken cancellationToken) {
        string json = await ProcessRunner.CaptureAsync(paths.Wsl, new[] {
            "--exec", "ffprobe", "-v", "error", "-show_entries",
            "format=duration:stream=index,codec_name,profile,channels,sample_rate:stream_tags=language,title",
            "-of", "json", PlayerPaths.ToWslPath(input)
        }, cancellationToken);
        using JsonDocument document = JsonDocument.Parse(json);
        JsonElement root = document.RootElement;
        JsonElement format = root.GetProperty("format");
        if (!format.TryGetProperty("duration", out JsonElement durationValue) ||
            !double.TryParse(durationValue.GetString(), System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out double duration)) {
            throw new InvalidDataException(
                "The input has no timeline duration. Use the original movie container instead of a raw TrueHD stream.");
        }
        List<AudioStreamInfo> streams = new();
        foreach (JsonElement stream in root.GetProperty("streams").EnumerateArray()) {
            string codec = stream.TryGetProperty("codec_name", out JsonElement codecValue)
                ? codecValue.GetString() ?? "" : "";
            string profile = stream.TryGetProperty("profile", out JsonElement profileValue)
                ? profileValue.GetString() ?? "" : "";
            AtmosCodec? atmosphere = codec switch {
                "truehd" when profile.Contains("Atmos", StringComparison.OrdinalIgnoreCase) => AtmosCodec.TrueHdAtmos,
                "eac3" when profile.Contains("Atmos", StringComparison.OrdinalIgnoreCase) => AtmosCodec.Eac3Joc,
                _ => null,
            };
            if (!atmosphere.HasValue) continue;
            JsonElement tags = stream.TryGetProperty("tags", out JsonElement tagValue) ? tagValue : default;
            string language = tags.ValueKind == JsonValueKind.Object && tags.TryGetProperty("language", out JsonElement lang)
                ? lang.GetString() ?? "" : "";
            string title = tags.ValueKind == JsonValueKind.Object && tags.TryGetProperty("title", out JsonElement name)
                ? name.GetString() ?? "" : "";
            streams.Add(new AudioStreamInfo(
                stream.GetProperty("index").GetInt32(), atmosphere.Value,
                stream.GetProperty("channels").GetInt32(),
                int.Parse(stream.GetProperty("sample_rate").GetString()!), language, title));
        }
        return new MediaInfo(duration, streams);
    }
}
