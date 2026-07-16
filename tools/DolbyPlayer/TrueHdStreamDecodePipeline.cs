using System.Diagnostics;
using Cavern.Format.Common;
using Cavern.Format.Container;

namespace DolbyPlayer;

internal sealed class TrueHdStreamDecodePipeline : IAtmosDecodePipeline {
    const double PrerollSeconds = 1;
    const double MaximumBufferedSeconds = 10;
    readonly PlayerPaths paths;
    readonly string input;
    readonly AudioStreamInfo stream;
    readonly double mediaDuration;
    readonly Pcm712Buffer output;
    CancellationTokenSource? producerCancellation;
    Task? producer;
    Exception? failure;

    public Exception? Failure => failure;
    public bool IsCompleted => producer?.IsCompleted == true;

    public TrueHdStreamDecodePipeline(PlayerPaths paths, string input, AudioStreamInfo stream,
                                      double mediaDuration, Pcm712Buffer output) {
        this.paths = paths;
        this.input = input;
        this.stream = stream;
        this.mediaDuration = mediaDuration;
        this.output = output;
    }

    public async Task RestartAsync(double mediaStartSeconds, CancellationToken cancellationToken) {
        await StopProducerAsync();
        failure = null;
        output.Reset(mediaStartSeconds);
        producerCancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        producer = ProduceAsync(mediaStartSeconds, producerCancellation.Token);
    }

    public async Task WaitForPrebufferAsync(double seconds, CancellationToken cancellationToken) {
        long frames = (long)(Math.Min(seconds, Math.Max(0, mediaDuration - output.MediaPositionSeconds)) *
            Pcm712Buffer.SampleRate);
        while (output.BufferedFrames < frames) {
            if (failure != null) throw new InvalidOperationException("TrueHD Atmos decoder failed.", failure);
            if (producer?.IsCompleted == true) break;
            cancellationToken.ThrowIfCancellationRequested();
            await Task.Delay(10, cancellationToken);
        }
    }

    async Task ProduceAsync(double requestedStart, CancellationToken cancellationToken) {
        Process? decoder = null;
        MatroskaReader? container = null;
        TrueHdObjectRenderer? renderer = null;
        Task? pump = null;
        try {
            double extractionStart = Math.Max(0, requestedStart - PrerollSeconds);
            container = new MatroskaReader(input);
            if (stream.Index < 0 || stream.Index >= container.Tracks.Length) {
                throw new InvalidDataException($"Track index 0:{stream.Index} is outside the Matroska track table.");
            }
            Track track = container.Tracks[stream.Index];
            if (track.Format != Codec.TrueHD) {
                throw new InvalidDataException($"Matroska track 0:{stream.Index} is {track.Format}, not TrueHD.");
            }
            container.Seek(extractionStart);
            while (track.IsNextBlockAvailable() && !track.IsNextBlockKeyframe()) track.ReadNextBlock();
            double decodedStart = track.GetNextBlockOffset();
            if (decodedStart < 0 || decodedStart > requestedStart + .001) {
                throw new InvalidDataException(
                    $"Could not find a TrueHD major sync before {requestedStart:F3} s (found {decodedStart:F3} s)." );
            }
            long discardFrames = (long)Math.Round((requestedStart - decodedStart) * Pcm712Buffer.SampleRate);
            long maximumFrames = (long)Math.Ceiling(
                Math.Max(0, mediaDuration - requestedStart) * Pcm712Buffer.SampleRate);

            decoder = StartDecoder();
            using CancellationTokenRegistration registration = cancellationToken.Register(() => {
                Kill(decoder);
            });
            Task<string> decoderError = decoder.StandardError.ReadToEndAsync(cancellationToken);
            pump = PumpTrackAsync(track, decoder.StandardInput.BaseStream, cancellationToken);

            TrueHdStreamProtocol protocol = new(decoder.StandardOutput.BaseStream);
            await protocol.ReadHeaderAsync(cancellationToken);
            bool eos = false;
            while (!eos) {
                while (output.BufferedFrames > MaximumBufferedSeconds * Pcm712Buffer.SampleRate) {
                    await Task.Delay(10, cancellationToken);
                }
                TrueHdRecord record = await protocol.ReadRecordAsync(cancellationToken);
                switch (record.Kind) {
                    case TrueHdRecordKind.Config:
                        if (record.Config == null) throw new InvalidDataException("Empty TrueHD CONFIG record.");
                        renderer = new TrueHdObjectRenderer(record.Config, output, discardFrames, maximumFrames);
                        break;
                    case TrueHdRecordKind.Reconfigure:
                        if (renderer == null || record.Config == null) {
                            throw new InvalidDataException("TrueHD RECONFIG arrived before CONFIG.");
                        }
                        renderer.Reconfigure(record.Config);
                        break;
                    case TrueHdRecordKind.Metadata:
                        if (renderer == null || record.Metadata == null) {
                            throw new InvalidDataException("TrueHD META arrived before CONFIG.");
                        }
                        renderer.AddMetadata(record.Metadata);
                        break;
                    case TrueHdRecordKind.Pcm:
                        if (renderer == null || record.Pcm == null) {
                            throw new InvalidDataException("TrueHD PCM arrived before CONFIG.");
                        }
                        renderer.AddPcm(record.SamplePosition, record.Pcm, record.PcmFrames);
                        break;
                    case TrueHdRecordKind.EndOfStream:
                        renderer?.Flush();
                        output.MarkCompleted();
                        eos = true;
                        break;
                }
            }

            await pump;
            await decoder.WaitForExitAsync(cancellationToken);
            string decoderMessage = await decoderError;
            if (decoder.ExitCode != 0) throw new InvalidOperationException(
                $"truehd-stream failed with {decoder.ExitCode}: {decoderMessage.Trim()}");
        } catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) {
        } catch (Exception exception) {
            failure = exception;
            Kill(decoder);
        } finally {
            Kill(decoder);
            if (pump != null) {
                try { await pump; } catch when (failure != null || cancellationToken.IsCancellationRequested) { }
            }
            renderer?.Dispose();
            container?.Dispose();
            decoder?.Dispose();
        }
    }

    Process StartDecoder() {
        ProcessStartInfo start = new(paths.TrueHdStream) {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
        };
        foreach (string argument in new[] {
            "-", "--output", "-", "--presentation", "3"
        }) start.ArgumentList.Add(argument);
        start.RedirectStandardInput = true;
        start.RedirectStandardOutput = true;
        return Process.Start(start) ?? throw new InvalidOperationException("Could not start truehd-stream.");
    }

    static async Task PumpTrackAsync(Track track, Stream output, CancellationToken cancellationToken) {
        try {
            while (track.IsNextBlockAvailable()) {
                cancellationToken.ThrowIfCancellationRequested();
                byte[] block = track.ReadNextBlock();
                if (block is { Length: > 0 }) await output.WriteAsync(block, cancellationToken);
            }
        } finally {
            await output.DisposeAsync();
        }
    }

    static void Kill(Process? process) {
        if (process?.HasExited == false) {
            try { process.Kill(true); } catch { }
        }
    }

    async Task StopProducerAsync() {
        if (producerCancellation == null) return;
        producerCancellation.Cancel();
        try { if (producer != null) await producer; } catch (OperationCanceledException) { }
        producerCancellation.Dispose();
        producerCancellation = null;
        producer = null;
    }

    public async ValueTask DisposeAsync() => await StopProducerAsync();
}
