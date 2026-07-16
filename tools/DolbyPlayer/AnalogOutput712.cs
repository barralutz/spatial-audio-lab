using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace DolbyPlayer;

internal sealed class AnalogOutput712 : IDisposable {
    static readonly Guid IeeeFloatSubFormat =
        new("00000003-0000-0010-8000-00aa00389b71");
    readonly MMDeviceEnumerator enumerator = new();
    readonly MMDevice rearDevice;
    readonly MMDevice heightDevice;
    readonly WasapiOut rearOutput;
    readonly WasapiOut heightOutput;
    readonly int rearBlockAlign;
    readonly int heightBlockAlign;
    long rearClockBase;
    long heightClockBase;
    double mediaClockBase;
    bool disposed;

    public string RearName => rearDevice.FriendlyName;
    public string HeightName => heightDevice.FriendlyName;
    public PlaybackState State => rearOutput.PlaybackState;
    public long RearDeviceBytes => rearOutput.GetPosition();
    public long HeightDeviceBytes => heightOutput.GetPosition();
    public double MediaPositionSeconds => mediaClockBase + Math.Min(
        ClockSeconds(rearOutput.GetPosition() - rearClockBase, rearBlockAlign),
        ClockSeconds(heightOutput.GetPosition() - heightClockBase, heightBlockAlign));
    public double ClockSkewMilliseconds => 1000 * (
        ClockSeconds(rearOutput.GetPosition() - rearClockBase, rearBlockAlign) -
        ClockSeconds(heightOutput.GetPosition() - heightClockBase, heightBlockAlign));

    public AnalogOutput712(Pcm712Buffer buffer, string rearFilter, string heightFilter,
                           float gain, int latencyMilliseconds = 30) {
        rearDevice = SelectDevice(rearFilter);
        heightDevice = SelectDevice(heightFilter);
        WaveFormat rearFormat = ValidateMixFormat(rearDevice, 8);
        WaveFormat heightFormat = ValidateMixFormat(heightDevice, 2);
        rearBlockAlign = rearFormat.BlockAlign;
        heightBlockAlign = heightFormat.BlockAlign;

        rearOutput = new WasapiOut(rearDevice, AudioClientShareMode.Shared, true, latencyMilliseconds);
        heightOutput = new WasapiOut(heightDevice, AudioClientShareMode.Shared, true, latencyMilliseconds);
        rearOutput.Init(new ProjectedProvider(buffer, 0, 0, 8, rearFormat, gain));
        heightOutput.Init(new ProjectedProvider(buffer, 1, 8, 2, heightFormat, gain));
    }

    public void Play() {
        heightOutput.Play();
        rearOutput.Play();
    }

    public void ResetClock(double mediaPositionSeconds) {
        rearClockBase = rearOutput.GetPosition();
        heightClockBase = heightOutput.GetPosition();
        mediaClockBase = mediaPositionSeconds;
    }

    public void Pause() {
        rearOutput.Pause();
        heightOutput.Pause();
    }

    public void Stop() {
        rearOutput.Stop();
        heightOutput.Stop();
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
        if (format.SampleRate != Pcm712Buffer.SampleRate || format.Channels != channels ||
            format.BitsPerSample != 32 || !isFloat) {
            throw new InvalidOperationException(
                $"Incompatible mix format on {device.FriendlyName}: {format}");
        }
        return format;
    }

    static double ClockSeconds(long bytes, int blockAlign) =>
        Math.Max(0, bytes) / (double)(blockAlign * Pcm712Buffer.SampleRate);

    public void Dispose() {
        if (disposed) return;
        disposed = true;
        rearOutput.Dispose();
        heightOutput.Dispose();
        rearDevice.Dispose();
        heightDevice.Dispose();
        enumerator.Dispose();
    }

    sealed class ProjectedProvider : IWaveProvider {
        readonly Pcm712Buffer source;
        readonly int reader;
        readonly int firstChannel;
        readonly int channels;
        readonly float gain;
        float[] scratch = Array.Empty<float>();

        public WaveFormat WaveFormat { get; }

        public ProjectedProvider(Pcm712Buffer source, int reader, int firstChannel, int channels,
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
