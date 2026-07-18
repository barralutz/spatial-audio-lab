using DolbyPlayer;

try {
    return await MainAsync(args);
} catch (OperationCanceledException) {
    return 130;
} catch (Exception exception) {
    Console.Error.WriteLine(exception);
    return 1;
}

static async Task<int> MainAsync(string[] args) {
    if (args.Length < 2) {
        PrintUsage();
        return 64;
    }

    string command = args[0].ToLowerInvariant();
    string input = Path.GetFullPath(args[1]);
    if (!File.Exists(input)) throw new FileNotFoundException("Input media was not found.", input);
    PlayerPaths paths = new();

    using CancellationTokenSource cancellation = new();
    Console.CancelKeyPress += (_, eventArgs) => {
        eventArgs.Cancel = true;
        cancellation.Cancel();
    };

    if (command == "self-test-audio") {
        double seconds = args.Length >= 3 ? ParseDouble(args[2]) : 10;
        float gain = args.Length >= 4 ? (float)ParseDouble(args[3]) : 1;
        float[] pcm = AtmosChunkRenderer.Render(input, seconds);
        Pcm714Buffer queue = new();
        queue.Reset(0);
        queue.Append(pcm);
        using PcmSinkOutput714 output = new(queue, "1 - HISENSE (Virtual Audio Device", gain);
        Console.WriteLine($"PCM 7.1.4 sink: {output.OutputName}");
        output.Play();
        double duration = pcm.Length / (double)(Pcm714Buffer.Channels * Pcm714Buffer.SampleRate);
        while (queue.MediaPositionSeconds < duration) await Task.Delay(10, cancellation.Token);
        output.Stop();
        Console.WriteLine($"Played {queue.MediaPositionSeconds:F6} s; starvation={queue.StarvationFrames}.");
        return 0;
    }

    paths.Validate(command is "play" or "video-test");
    MediaInfo media = await MediaProbe.ReadAsync(paths, input, cancellation.Token);
    if (command == "inspect") {
        Console.WriteLine($"Duration: {media.DurationSeconds:F3} s");
        foreach (AudioStreamInfo stream in media.AtmosStreams) {
            Console.WriteLine($"0:{stream.Index} {stream.Codec} {stream.Channels}ch/{stream.SampleRate} Hz " +
                              $"{stream.Language} {stream.Title}".TrimEnd());
        }
        return media.AtmosStreams.Count == 0 ? 2 : 0;
    }
    if (command == "decode-test") {
        PlayerOptions testOptions = ParseOptions(args.Skip(2).ToArray());
        AudioStreamInfo testStream = media.Select(testOptions.AudioStreamIndex);
        Pcm714Buffer testQueue = new();
        await using IAtmosDecodePipeline testDecoder = testStream.Codec == AtmosCodec.Eac3Joc
            ? new Eac3JocDecodePipeline(input, testStream, media.DurationSeconds, testQueue)
            : new TrueHdStreamDecodePipeline(paths, input, testStream, media.DurationSeconds, testQueue);
        System.Diagnostics.Stopwatch timer = System.Diagnostics.Stopwatch.StartNew();
        await testDecoder.RestartAsync(testOptions.StartSeconds, cancellation.Token);
        await testDecoder.WaitForPrebufferAsync(5, cancellation.Token);
        if (testDecoder.Failure != null) throw new InvalidOperationException(
            "Atmos decode test failed.", testDecoder.Failure);
        Console.WriteLine($"Decoded {testQueue.BufferedFrames / (double)Pcm714Buffer.SampleRate:F3} s " +
                          $"of {testStream.Codec} in {timer.Elapsed.TotalSeconds:F3} s.");
        PrintChannelLevels(testQueue.SnapshotBuffered());
        return 0;
    }
    if (command == "audio-test") {
        PlayerOptions testOptions = ParseOptions(args.Skip(2).ToArray());
        AudioStreamInfo testStream = media.Select(testOptions.AudioStreamIndex);
        Pcm714Buffer testQueue = new();
        await using IAtmosDecodePipeline testDecoder = testStream.Codec == AtmosCodec.Eac3Joc
            ? new Eac3JocDecodePipeline(input, testStream, media.DurationSeconds, testQueue)
            : new TrueHdStreamDecodePipeline(paths, input, testStream, media.DurationSeconds, testQueue);
        using PcmSinkOutput714 testOutput = new(
            testQueue, testOptions.SinkFilter, testOptions.Gain);
        double seconds = testOptions.StopAfterSeconds ?? 10;
        double stop = Math.Min(media.DurationSeconds, testOptions.StartSeconds + seconds);
        await testDecoder.RestartAsync(testOptions.StartSeconds, cancellation.Token);
        await testDecoder.WaitForPrebufferAsync(Math.Min(2.5, seconds), cancellation.Token);
        if (testDecoder.Failure != null) throw new InvalidOperationException(
            "Atmos audio test decode failed.", testDecoder.Failure);
        testOutput.ResetClock(testOptions.StartSeconds);
        testOutput.Play();
        while (testOutput.MediaPositionSeconds < stop && !cancellation.IsCancellationRequested) {
            if (testDecoder.Failure != null) throw new InvalidOperationException(
                "Atmos audio test decode failed.", testDecoder.Failure);
            if (testDecoder.IsCompleted && testQueue.BufferedFrames == 0 &&
                testQueue.MediaPositionSeconds - testOutput.MediaPositionSeconds < .05) break;
            await Task.Delay(20, cancellation.Token);
        }
        double finalPosition = testOutput.MediaPositionSeconds;
        double clockSkew = testOutput.ClockSkewMilliseconds;
        testOutput.Stop();
        Console.WriteLine($"Audio test passed: {finalPosition - testOptions.StartSeconds:F3} s, " +
                          $"starvation={testQueue.StarvationFrames}, clock delta={clockSkew:F3} ms.");
        return 0;
    }
    if (command == "video-test") {
        PlayerOptions testOptions = ParseOptions(args.Skip(2).ToArray());
        await using MpvController mpv = new(paths.Mpv, input, testOptions.StartSeconds, true);
        await mpv.StartAsync(cancellation.Token);
        System.Text.Json.JsonElement tracks = await mpv.GetTrackListAsync(cancellation.Token);
        int subtitleCount = 0;
        int? firstSubtitle = null;
        foreach (System.Text.Json.JsonElement track in tracks.EnumerateArray()) {
            if (track.GetProperty("type").GetString() != "sub") continue;
            ++subtitleCount;
            firstSubtitle ??= track.GetProperty("id").GetInt32();
        }
        if (firstSubtitle.HasValue) {
            await mpv.SetSubtitleAsync(firstSubtitle.Value, cancellation.Token);
            if (await mpv.GetSubtitleAsync(cancellation.Token) != firstSubtitle) {
                throw new InvalidOperationException("mpv did not select the requested subtitle track.");
            }
            await Task.Delay(100, cancellation.Token);
        }
        double initial = (await mpv.GetTimeAsync(cancellation.Token)) ?? testOptions.StartSeconds;
        if (!await mpv.GetPausedAsync(cancellation.Token)) {
            throw new InvalidOperationException("mpv did not start paused.");
        }
        await mpv.SetPausedAsync(false, cancellation.Token);
        await Task.Delay(750, cancellation.Token);
        double advanced = (await mpv.GetTimeAsync(cancellation.Token)) ?? initial;
        await mpv.SetPausedAsync(true, cancellation.Token);
        double target = Math.Min(media.DurationSeconds - .1, initial + 10);
        await mpv.SeekAsync(target, cancellation.Token);
        for (int retry = 0; retry < 100 && await mpv.GetSeekingAsync(cancellation.Token); ++retry) {
            await Task.Delay(20, cancellation.Token);
        }
        double sought = (await mpv.GetTimeAsync(cancellation.Token)) ?? -1;
        if (advanced <= initial + .25 || Math.Abs(sought - target) > .20) {
            throw new InvalidOperationException(
                $"mpv clock test failed: initial={initial:F3}, advanced={advanced:F3}, target={target:F3}, sought={sought:F3}.");
        }
        Console.WriteLine($"mpv IPC passed: {initial:F3} -> {advanced:F3} s, seek -> {sought:F3} s, " +
                          $"subtitles={subtitleCount}.");
        return 0;
    }
    if (command != "play") {
        PrintUsage();
        return 64;
    }

    PlayerOptions options = ParseOptions(args.Skip(2).ToArray());
    AudioStreamInfo selected = media.Select(options.AudioStreamIndex);
    await PlayerCoordinator.PlayAsync(paths, input, media, selected, options, cancellation.Token);
    return 0;
}

static PlayerOptions ParseOptions(string[] args) {
    int? stream = null;
    double start = 0;
    float gain = 1;
    string sink = "1 - HISENSE (Virtual Audio Device";
    double avDelay = 0;
    double? stopAfter = null;
    bool controlTest = false;
    for (int index = 0; index < args.Length; ++index) {
        string value = index + 1 < args.Length ? args[index + 1] : "";
        switch (args[index]) {
            case "--audio-track": stream = int.Parse(value); ++index; break;
            case "--start": start = ParseDouble(value); ++index; break;
            case "--gain": gain = (float)ParseDouble(value); ++index; break;
            case "--sink": sink = value; ++index; break;
            case "--av-delay-ms": avDelay = ParseDouble(value); ++index; break;
            case "--stop-after": stopAfter = ParseDouble(value); ++index; break;
            case "--control-test": controlTest = true; break;
            default: throw new ArgumentException($"Unknown option: {args[index]}");
        }
    }
    if (start < 0 || gain < 0 || gain > 1 || stopAfter <= 0) {
        throw new ArgumentOutOfRangeException(nameof(args));
    }
    return new PlayerOptions(stream, start, gain, sink, avDelay, stopAfter, controlTest);
}

static double ParseDouble(string value) => double.Parse(
    value, System.Globalization.CultureInfo.InvariantCulture);

static void PrintChannelLevels(float[] pcm) {
    string[] names = {
        "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR", "TFL", "TFR", "TBL", "TBR"
    };
    double[] squares = new double[Pcm714Buffer.Channels];
    float[] peaks = new float[Pcm714Buffer.Channels];
    int frames = pcm.Length / Pcm714Buffer.Channels;
    for (int frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < Pcm714Buffer.Channels; ++channel) {
            float value = pcm[frame * Pcm714Buffer.Channels + channel];
            squares[channel] += value * value;
            peaks[channel] = Math.Max(peaks[channel], Math.Abs(value));
        }
    }
    for (int channel = 0; channel < Pcm714Buffer.Channels; ++channel) {
        double rms = frames == 0 ? 0 : Math.Sqrt(squares[channel] / frames);
        static string Db(double value) => value > 0 ? $"{20 * Math.Log10(value),7:F2}" : "   -inf";
        Console.WriteLine($"  {names[channel],3}: RMS {Db(rms)} dBFS, peak {Db(peaks[channel])} dBFS");
    }
}

static void PrintUsage() {
    Console.Error.WriteLine("SpatialAudioLab.Cinema inspect <media>");
    Console.Error.WriteLine("SpatialAudioLab.Cinema decode-test <media> [--audio-track N] [--start seconds]");
    Console.Error.WriteLine("SpatialAudioLab.Cinema audio-test <media> [--start seconds] [--gain 0..1] " +
                            "[--sink endpoint] [--stop-after seconds]");
    Console.Error.WriteLine("SpatialAudioLab.Cinema video-test <media> [--start seconds]");
    Console.Error.WriteLine("SpatialAudioLab.Cinema play <media> [--audio-track N] [--start seconds] [--gain 0..1] " +
                            "[--sink endpoint] [--av-delay-ms N] [--stop-after seconds]");
    Console.Error.WriteLine("SpatialAudioLab.Cinema self-test-audio <input.eac3|input.atmos> [seconds] [gain]");
}
