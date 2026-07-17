using Cavern;
using Cavern.Channels;
using Cavern.Format;
using Cavern.Format.Common;
using Cavern.Format.Container;
using Cavern.Format.Renderers;
using Cavern.Utilities;

namespace DolbyPlayer;

internal sealed class Eac3JocDecodePipeline : IAtmosDecodePipeline {
    const int UpdateRate = 64;
    const int UpdatesPerBatch = 24;
    const double PrerollSeconds = .30;
    const double MaximumBufferedSeconds = 10;
    static int channelsInitialized;

    readonly string input;
    readonly AudioStreamInfo stream;
    readonly double mediaDuration;
    readonly Pcm714Buffer output;
    CancellationTokenSource? producerCancellation;
    Task? producer;
    Exception? failure;

    public Exception? Failure => failure;
    public bool IsCompleted => producer?.IsCompleted == true;

    public Eac3JocDecodePipeline(string input, AudioStreamInfo stream, double mediaDuration,
                                 Pcm714Buffer output) {
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
        producer = Task.Run(() => Produce(mediaStartSeconds, producerCancellation.Token),
            producerCancellation.Token);
    }

    public async Task WaitForPrebufferAsync(double seconds, CancellationToken cancellationToken) {
        long frames = (long)(Math.Min(seconds, Math.Max(0, mediaDuration - output.MediaPositionSeconds)) *
            Pcm714Buffer.SampleRate);
        while (output.BufferedFrames < frames) {
            if (failure != null) throw new InvalidOperationException("E-AC-3 JOC decoder failed.", failure);
            if (producer?.IsCompleted == true) break;
            cancellationToken.ThrowIfCancellationRequested();
            await Task.Delay(10, cancellationToken);
        }
    }

    void Produce(double requestedStart, CancellationToken cancellationToken) {
        AudioReader? reader = null;
        Renderer? renderer = null;
        MatroskaReader? container = null;
        try {
            if (Interlocked.Exchange(ref channelsInitialized, 1) == 0) {
                Listener.ReplaceChannels(ChannelPrototype.ToLayout(ChannelPrototype.ref714));
            }

            double prerollTarget = Math.Max(0, requestedStart - PrerollSeconds);
            double decodedStart;
            if (Path.GetExtension(input).Equals(".eac3", StringComparison.OrdinalIgnoreCase) ||
                Path.GetExtension(input).Equals(".ec3", StringComparison.OrdinalIgnoreCase)) {
                EnhancedAC3Reader eac3 = new(input);
                eac3.ReadHeader();
                if (prerollTarget > 0) eac3.Seek((long)(prerollTarget * Pcm714Buffer.SampleRate));
                reader = eac3;
                decodedStart = prerollTarget;
            } else {
                container = new MatroskaReader(input);
                if (stream.Index < 0 || stream.Index >= container.Tracks.Length) {
                    throw new InvalidDataException($"Track index 0:{stream.Index} is outside the Matroska track table.");
                }
                Track track = container.Tracks[stream.Index];
                if (track.Format != Codec.EnhancedAC3) {
                    throw new InvalidDataException($"Matroska track 0:{stream.Index} is {track.Format}, not E-AC-3.");
                }
                if (prerollTarget > 0) container.Seek(prerollTarget);
                decodedStart = Math.Max(0, track.GetNextBlockOffset());
                AudioTrackReader trackReader = new(track);
                trackReader.ReadHeader();
                reader = trackReader;
            }

            renderer = reader.GetRenderer();
            if (!renderer.HasObjects) throw new InvalidDataException("The E-AC-3 stream has no Atmos objects.");
            Listener listener = new(false) {
                SampleRate = Pcm714Buffer.SampleRate,
                UpdateRate = UpdateRate,
                AudioQuality = QualityModes.Perfect,
                Volume = .707f,
            };
            listener.AttachSources(renderer.Objects);

            long discardFrames = Math.Max(0,
                (long)Math.Round((requestedStart - decodedStart) * Pcm714Buffer.SampleRate));
            long remainingFrames = Math.Max(0,
                (long)Math.Ceiling((mediaDuration - requestedStart) * Pcm714Buffer.SampleRate));
            while (remainingFrames > 0) {
                cancellationToken.ThrowIfCancellationRequested();
                while (output.BufferedFrames > MaximumBufferedSeconds * Pcm714Buffer.SampleRate) {
                    Thread.Sleep(10);
                    cancellationToken.ThrowIfCancellationRequested();
                }

                float[] block = listener.Render(UpdatesPerBatch);
                int blockFrames = block.Length / Pcm714Buffer.Channels;
                int firstFrame = (int)Math.Min(discardFrames, blockFrames);
                discardFrames -= firstFrame;
                int frames = (int)Math.Min(blockFrames - firstFrame, remainingFrames);
                if (frames > 0) {
                    float[] copy = new float[frames * Pcm714Buffer.Channels];
                    Array.Copy(block, firstFrame * Pcm714Buffer.Channels,
                        copy, 0, copy.Length);
                    output.Append(copy);
                    remainingFrames -= frames;
                }
            }
            output.MarkCompleted();
            listener.DetachAllSources();
        } catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) {
        } catch (Exception exception) {
            failure = exception;
        } finally {
            renderer?.Dispose();
            reader?.Dispose();
            container?.Dispose();
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
