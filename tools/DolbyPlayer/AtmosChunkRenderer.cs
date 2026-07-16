using Cavern;
using Cavern.Channels;
using Cavern.Format;
using Cavern.Format.Common;
using Cavern.Format.Renderers;
using Cavern.Utilities;

namespace DolbyPlayer;

internal static class AtmosChunkRenderer {
    const int OutputChannels = 10;
    const int UpdateRate = 64;
    static int channelsInitialized;

    public static float[] Render(string input, double maximumSeconds = double.PositiveInfinity) {
        if (Interlocked.Exchange(ref channelsInitialized, 1) == 0) {
            Listener.ReplaceChannels(ChannelPrototype.ToLayout(ChannelPrototype.ref712));
        }

        using AudioReader reader = AudioReader.Open(input);
        reader.ReadHeader();
        if (reader is AudioTrackReader trackReader && trackReader.track.Format == Codec.TrueHD) {
            throw new NotSupportedException("TrueHD must be converted to presentation 3 DAMF first.");
        }
        if (reader.SampleRate != Pcm712Buffer.SampleRate) {
            throw new NotSupportedException($"Only 48 kHz Atmos is supported, got {reader.SampleRate} Hz.");
        }

        using Renderer renderer = reader.GetRenderer();
        long maximumFrames = double.IsFinite(maximumSeconds)
            ? (long)Math.Ceiling(maximumSeconds * reader.SampleRate)
            : long.MaxValue;
        long outputFrames = Math.Min(reader.Length, maximumFrames);
        if (outputFrames <= 0 || outputFrames > int.MaxValue / OutputChannels) {
            throw new InvalidDataException($"Invalid or oversized chunk length: {outputFrames} frames.");
        }

        Listener listener = new(false) {
            SampleRate = reader.SampleRate,
            UpdateRate = UpdateRate,
            AudioQuality = QualityModes.Perfect,
            Volume = renderer.HasObjects ? .707f : 1,
        };
        listener.AttachSources(renderer.Objects);

        float[] result = new float[checked((int)outputFrames * OutputChannels)];
        long renderedFrames = 0;
        while (renderedFrames < outputFrames) {
            float[] block = listener.Render();
            int frames = (int)Math.Min(UpdateRate, outputFrames - renderedFrames);
            Array.Copy(block, 0, result, renderedFrames * OutputChannels, frames * OutputChannels);
            renderedFrames += frames;
        }
        listener.DetachAllSources();
        return result;
    }
}
