using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace DolbyPlayer;

internal sealed class PcmSinkOutput714 : IDisposable {
    static readonly Guid IeeeFloatSubFormat =
        new("00000003-0000-0010-8000-00aa00389b71");
    readonly MMDeviceEnumerator enumerator = new();
    readonly MMDevice sinkDevice;
    readonly WasapiOut sinkOutput;
    readonly int sinkBlockAlign;
    long sinkClockBase;
    double mediaClockBase;
    bool disposed;

    public string OutputName => sinkDevice.FriendlyName;
    public PlaybackState State => sinkOutput.PlaybackState;
    public long DeviceBytes => sinkOutput.GetPosition();
    public double MediaPositionSeconds => mediaClockBase +
        ClockSeconds(sinkOutput.GetPosition() - sinkClockBase, sinkBlockAlign);
    public double ClockSkewMilliseconds => 0;

    public PcmSinkOutput714(Pcm714Buffer buffer, string sinkFilter,
                            float gain, int latencyMilliseconds = 30) {
        sinkDevice = SelectDevice(sinkFilter);
        WaveFormat sinkFormat = ValidateMixFormat(sinkDevice, Pcm714Buffer.Channels);
        sinkBlockAlign = sinkFormat.BlockAlign;

        sinkOutput = new WasapiOut(
            sinkDevice, AudioClientShareMode.Shared, true, latencyMilliseconds);
        sinkOutput.Init(new ProjectedProvider(
            buffer, 0, 0, Pcm714Buffer.Channels, sinkFormat, gain));
    }

    public void Play() => sinkOutput.Play();

    public void ResetClock(double mediaPositionSeconds) {
        sinkClockBase = sinkOutput.GetPosition();
        mediaClockBase = mediaPositionSeconds;
    }

    public void Pause() {
        sinkOutput.Pause();
    }

    public void Stop() {
        sinkOutput.Stop();
    }

    MMDevice SelectDevice(string filter) {
        MMDevice[] matches = enumerator
            .EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active)
            .Where(device => device.FriendlyName.Contains(filter, StringComparison.OrdinalIgnoreCase))
            .ToArray();
        if (matches.Length != 1) {
            string found = string.Join(", ", matches.Select(device => device.FriendlyName));
            throw new InvalidOperationException(
                $"Endpoint filter '{filter}' matched {matches.Length} devices: {found}");
        }
        return matches[0];
    }

    static WaveFormat ValidateMixFormat(MMDevice device, int channels) {
        WaveFormat format = device.AudioClient.MixFormat;
        bool isFloat = format.Encoding == WaveFormatEncoding.IeeeFloat ||
            format is WaveFormatExtensible extensible &&
            extensible.SubFormat == IeeeFloatSubFormat;
        if (format.SampleRate != Pcm714Buffer.SampleRate || format.Channels != channels ||
            format.BitsPerSample != 32 || !isFloat) {
            throw new InvalidOperationException(
                $"Incompatible mix format on {device.FriendlyName}: {format}");
        }
        return format;
    }

    static double ClockSeconds(long bytes, int blockAlign) =>
        Math.Max(0, bytes) / (double)(blockAlign * Pcm714Buffer.SampleRate);

    public void Dispose() {
        if (disposed) return;
        disposed = true;
        sinkOutput.Dispose();
        sinkDevice.Dispose();
        enumerator.Dispose();
    }

    sealed class ProjectedProvider : IWaveProvider {
        readonly Pcm714Buffer source;
        readonly int reader;
        readonly int firstChannel;
        readonly int channels;
        readonly float gain;
        float[] scratch = Array.Empty<float>();

        public WaveFormat WaveFormat { get; }

        public ProjectedProvider(Pcm714Buffer source, int reader, int firstChannel, int channels,
                                 WaveFormat waveFormat, float gain) {
            this.source = source;
            this.reader = reader;
            this.firstChannel = firstChannel;
            this.channels = channels;
            this.gain = gain;
            WaveFormat = waveFormat;
        }

        public int Read(byte[] buffer, int offset, int count) {
            int samples = count / sizeof(float);
            if (scratch.Length != samples) scratch = new float[samples];
            source.ReadProjected(reader, firstChannel, channels, scratch, gain);
            Buffer.BlockCopy(scratch, 0, buffer, offset, samples * sizeof(float));
            return count;
        }
    }
}
