using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace DolbyPlayer;

internal sealed class MpvController : IAsyncDisposable {
    readonly string executable;
    readonly string input;
    readonly double startSeconds;
    readonly bool headless;
    readonly string pipeName = $"dolby-player-{Environment.ProcessId}-{Guid.NewGuid():N}";
    readonly SemaphoreSlim requestLock = new(1, 1);
    Process? process;
    NamedPipeClientStream? pipe;
    StreamReader? reader;
    StreamWriter? writer;
    int nextRequest;

    public bool HasExited => process?.HasExited != false;

    public MpvController(string executable, string input, double startSeconds, bool headless = false) {
        this.executable = executable;
        this.input = input;
        this.startSeconds = startSeconds;
        this.headless = headless;
    }

    public async Task StartAsync(CancellationToken cancellationToken) {
        ProcessStartInfo start = new(executable) {
            UseShellExecute = false,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            CreateNoWindow = false,
        };
        string startText = startSeconds.ToString("F6", System.Globalization.CultureInfo.InvariantCulture);
        List<string> arguments = new() {
            "--no-audio", "--pause=yes", "--keep-open=no",
            "--hwdec=auto-safe", "--title=DolbyPlayer Atmos 7.1.4",
            $"--start={startText}", $"--input-ipc-server=\\\\.\\pipe\\{pipeName}", input
        };
        arguments.Insert(2, headless ? "--vo=null" : "--force-window=yes");
        foreach (string argument in arguments) start.ArgumentList.Add(argument);
        process = Process.Start(start) ?? throw new InvalidOperationException("Could not start mpv.");
        _ = process.StandardError.ReadToEndAsync();
        _ = process.StandardOutput.ReadToEndAsync();

        pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut,
            PipeOptions.Asynchronous);
        using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(10));
        await pipe.ConnectAsync(timeout.Token);
        reader = new StreamReader(pipe, new UTF8Encoding(false), false, 4096, true);
        writer = new StreamWriter(pipe, new UTF8Encoding(false), 4096, true) { AutoFlush = true };
        for (int retry = 0; retry < 200; ++retry) {
            if ((await GetTimeAsync(timeout.Token)).HasValue) return;
            if (HasExited) throw new InvalidOperationException("mpv exited before loading the media file.");
            await Task.Delay(50, timeout.Token);
        }
        throw new TimeoutException("mpv did not finish loading the media file.");
    }

    public Task<double?> GetTimeAsync(CancellationToken cancellationToken) =>
        GetPropertyAsync<double?>("time-pos", cancellationToken);

    public Task<bool> GetPausedAsync(CancellationToken cancellationToken) =>
        GetPropertyAsync<bool>("pause", cancellationToken);

    public Task<bool> GetSeekingAsync(CancellationToken cancellationToken) =>
        GetPropertyAsync<bool>("seeking", cancellationToken);

    public Task SetPausedAsync(bool value, CancellationToken cancellationToken) =>
        CommandAsync(new object[] { "set_property", "pause", value }, cancellationToken);

    public Task SeekAsync(double seconds, CancellationToken cancellationToken) =>
        CommandAsync(new object[] { "seek", seconds, "absolute+exact" }, cancellationToken);

    public Task SetSpeedAsync(double speed, CancellationToken cancellationToken) =>
        CommandAsync(new object[] { "set_property", "speed", speed }, cancellationToken);

    public Task<JsonElement> GetTrackListAsync(CancellationToken cancellationToken) =>
        RequestAsync(new object[] { "get_property", "track-list" }, cancellationToken);

    public Task SetSubtitleAsync(int id, CancellationToken cancellationToken) =>
        CommandAsync(new object[] { "set_property", "sid", id }, cancellationToken);

    public Task<int?> GetSubtitleAsync(CancellationToken cancellationToken) =>
        GetPropertyAsync<int?>("sid", cancellationToken);

    async Task<T> GetPropertyAsync<T>(string name, CancellationToken cancellationToken) {
        JsonElement data;
        try {
            data = await RequestAsync(new object[] { "get_property", name }, cancellationToken);
        } catch (InvalidOperationException exception) when (
            exception.Message.Contains("property unavailable", StringComparison.OrdinalIgnoreCase)) {
            return default!;
        }
        if (data.ValueKind == JsonValueKind.Null) return default!;
        return data.Deserialize<T>()!;
    }

    async Task CommandAsync(object[] command, CancellationToken cancellationToken) =>
        _ = await RequestAsync(command, cancellationToken);

    async Task<JsonElement> RequestAsync(object[] command, CancellationToken cancellationToken) {
        if (writer == null || reader == null) throw new InvalidOperationException("mpv IPC is not connected.");
        await requestLock.WaitAsync(cancellationToken);
        try {
            int requestId = Interlocked.Increment(ref nextRequest);
            string json = JsonSerializer.Serialize(new { command, request_id = requestId });
            await writer.WriteLineAsync(json.AsMemory(), cancellationToken);
            while (true) {
                string? line = await reader.ReadLineAsync(cancellationToken);
                if (line == null) throw new EndOfStreamException("mpv IPC closed.");
                using JsonDocument response = JsonDocument.Parse(line);
                JsonElement root = response.RootElement;
                if (!root.TryGetProperty("request_id", out JsonElement id) || id.GetInt32() != requestId) continue;
                string error = root.GetProperty("error").GetString() ?? "unknown";
                if (error != "success") throw new InvalidOperationException($"mpv command failed: {error}");
                return root.TryGetProperty("data", out JsonElement data) ? data.Clone() : default;
            }
        } finally {
            requestLock.Release();
        }
    }

    public async ValueTask DisposeAsync() {
        try {
            if (!HasExited && writer != null) {
                await CommandAsync(new object[] { "quit" }, CancellationToken.None);
            }
        } catch { }
        try { writer?.Dispose(); } catch { }
        try { reader?.Dispose(); } catch { }
        try { pipe?.Dispose(); } catch { }
        if (process != null) {
            if (!process.HasExited) {
                try { process.Kill(true); } catch { }
            }
            process.Dispose();
        }
        requestLock.Dispose();
    }
}
