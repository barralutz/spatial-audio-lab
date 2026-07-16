using System.Diagnostics;

namespace DolbyPlayer;

internal sealed class Pcm712Buffer {
    public const int SampleRate = 48_000;
    public const int Channels = 10;

    readonly object gate = new();
    readonly List<float> samples = new();
    readonly long[] cursors = new long[2];
    long baseFrame;
    double mediaStartSeconds;
    long starvationFrames;
    bool completed;

    public double MediaPositionSeconds {
        get {
            lock (gate) {
                return mediaStartSeconds + Math.Min(cursors[0], cursors[1]) / (double)SampleRate;
            }
        }
    }

    public long BufferedFrames {
        get {
            lock (gate) {
                return EndFrameLocked() - Math.Max(cursors[0], cursors[1]);
            }
        }
    }

    public long StarvationFrames {
        get { lock (gate) return starvationFrames; }
    }

    public float[] SnapshotBuffered() {
        lock (gate) {
            long firstFrame = Math.Max(cursors[0], cursors[1]);
            int firstSample = checked((int)((firstFrame - baseFrame) * Channels));
            int count = samples.Count - firstSample;
            float[] result = new float[count];
            samples.CopyTo(firstSample, result, 0, count);
            return result;
        }
    }

    public void Reset(double startSeconds) {
        lock (gate) {
            samples.Clear();
            baseFrame = 0;
            cursors[0] = 0;
            cursors[1] = 0;
            mediaStartSeconds = startSeconds;
            starvationFrames = 0;
            completed = false;
        }
    }

    public void MarkCompleted() {
        lock (gate) completed = true;
    }

    public void Append(float[] interleaved) {
        if (interleaved.Length % Channels != 0) {
            throw new ArgumentException("PCM 7.1.2 must contain complete 10-channel frames.", nameof(interleaved));
        }
        lock (gate) {
            samples.AddRange(interleaved);
        }
    }

    public int ReadProjected(int reader, int firstChannel, int channelCount, Span<float> destination,
                             float gain) {
        if ((uint)reader >= cursors.Length || firstChannel < 0 ||
            firstChannel + channelCount > Channels || destination.Length % channelCount != 0) {
            throw new ArgumentOutOfRangeException(nameof(reader));
        }

        int frames = destination.Length / channelCount;
        int copiedFrames = 0;
        lock (gate) {
            long endFrame = EndFrameLocked();
            long cursor = cursors[reader];
            for (int frame = 0; frame < frames; ++frame) {
                bool available = cursor < endFrame;
                for (int channel = 0; channel < channelCount; ++channel) {
                    float value = 0;
                    if (available) {
                        int source = checked((int)((cursor - baseFrame) * Channels + firstChannel + channel));
                        value = Math.Clamp(samples[source] * gain, -1, 1);
                    }
                    destination[frame * channelCount + channel] = value;
                }
                if (available) {
                    ++cursor;
                    ++copiedFrames;
                } else if (!completed) {
                    ++starvationFrames;
                }
            }
            cursors[reader] = cursor;
            DiscardConsumedLocked();
        }
        return copiedFrames;
    }

    public async Task WaitForFramesAsync(long requiredFrames, CancellationToken cancellationToken) {
        Stopwatch timeout = Stopwatch.StartNew();
        while (BufferedFrames < requiredFrames) {
            cancellationToken.ThrowIfCancellationRequested();
            if (timeout.Elapsed > TimeSpan.FromSeconds(30)) {
                throw new TimeoutException($"Timed out waiting for {requiredFrames} buffered PCM frames.");
            }
            await Task.Delay(10, cancellationToken);
        }
    }

    long EndFrameLocked() => baseFrame + samples.Count / Channels;

    void DiscardConsumedLocked() {
        long consumed = Math.Min(cursors[0], cursors[1]) - baseFrame;
        if (consumed < 4_800 && consumed != samples.Count / Channels) return;
        int discardSamples = checked((int)(consumed * Channels));
        samples.RemoveRange(0, discardSamples);
        baseFrame += consumed;
    }
}
