using System.Numerics;
using Cavern;
using Cavern.Channels;
using Cavern.SpecialSources;
using Cavern.Utilities;

namespace DolbyPlayer;

internal sealed class TrueHdObjectRenderer : IDisposable {
    const int UpdateRate = 64;
    readonly Pcm714Buffer output;
    readonly long maximumOutputFrames;
    readonly List<float> pendingPcm = new();
    readonly List<TrueHdMetadataEvent>[] metadata;
    readonly int[] metadataCursor;
    readonly ObjectState[] stableState;
    readonly List<Source> sources;
    readonly Listener listener;
    readonly StreamMaster master;
    TrueHdStreamConfig config;
    float[][] nextPlanar;
    int pendingOffset;
    long nextInputSample = -1;
    long discardFrames;
    long emittedFrames;

    public long EmittedFrames => emittedFrames;

    public TrueHdObjectRenderer(TrueHdStreamConfig config, Pcm714Buffer output,
                                long discardFrames, long maximumOutputFrames) {
        if (config.SampleRate != Pcm714Buffer.SampleRate) {
            throw new NotSupportedException($"Only 48 kHz TrueHD is supported, got {config.SampleRate} Hz.");
        }
        this.config = config;
        this.output = output;
        this.discardFrames = discardFrames;
        this.maximumOutputFrames = maximumOutputFrames;
        metadata = Enumerable.Range(0, config.ChannelCount).Select(_ => new List<TrueHdMetadataEvent>()).ToArray();
        metadataCursor = new int[config.ChannelCount];
        stableState = Enumerable.Range(0, config.ChannelCount).Select(_ => ObjectState.Default).ToArray();
        nextPlanar = CreatePlanar(config.ChannelCount, UpdateRate);

        Listener.ReplaceChannels(ChannelPrototype.ToLayout(ChannelPrototype.ref714));
        master = new StreamMaster(_ => nextPlanar);
        sources = Enumerable.Range(0, config.ChannelCount)
            .Select(index => (Source)new StreamMasterSource(master, index) {
                DopplerLevel = 0,
                VolumeRolloff = Rolloffs.Disabled,
                SpatialBlend = 1,
            }).ToList();
        master.SetupSources(sources, Pcm714Buffer.SampleRate);
        listener = new Listener(false) {
            SampleRate = Pcm714Buffer.SampleRate,
            UpdateRate = UpdateRate,
            AudioQuality = QualityModes.Perfect,
            Volume = .707f,
        };
        ApplyConfiguration();
        listener.AttachSources(sources);
    }

    public void Reconfigure(TrueHdStreamConfig next) {
        if (next.SampleRate != config.SampleRate || next.ChannelCount != config.ChannelCount) {
            throw new NotSupportedException(
                $"A live TrueHD layout change from {config.ChannelCount} to {next.ChannelCount} elements is not supported.");
        }
        config = next;
        ApplyConfiguration();
    }

    public void AddMetadata(IReadOnlyList<TrueHdMetadataEvent> events) {
        foreach (TrueHdMetadataEvent item in events) {
            if ((uint)item.Element >= metadata.Length) {
                throw new InvalidDataException($"TrueHD metadata references missing element {item.Element}.");
            }
            metadata[item.Element].Add(item);
        }
    }

    public void AddPcm(long samplePosition, float[] pcm, int frames) {
        if (pcm.Length != checked(frames * config.ChannelCount)) {
            throw new InvalidDataException("TrueHD PCM channel count changed without a RECONFIG record.");
        }
        if (nextInputSample < 0) nextInputSample = samplePosition;
        long bufferedFrames = (pendingPcm.Count - pendingOffset) / config.ChannelCount;
        long expectedPosition = nextInputSample + bufferedFrames;
        int firstFrame = 0;
        if (samplePosition > expectedPosition) {
            int gap = checked((int)(samplePosition - expectedPosition));
            pendingPcm.AddRange(new float[checked(gap * config.ChannelCount)]);
        } else if (samplePosition < expectedPosition) {
            firstFrame = checked((int)Math.Min(frames, expectedPosition - samplePosition));
        }
        if (firstFrame < frames) {
            pendingPcm.AddRange(pcm.AsSpan(firstFrame * config.ChannelCount).ToArray());
        }
        RenderAvailable(false);
    }

    public void Flush() => RenderAvailable(true);

    void RenderAvailable(bool flush) {
        while (emittedFrames < maximumOutputFrames) {
            int availableFrames = (pendingPcm.Count - pendingOffset) / config.ChannelCount;
            if (availableFrames < UpdateRate && (!flush || availableFrames == 0)) break;
            int inputFrames = Math.Min(UpdateRate, availableFrames);
            nextPlanar = CreatePlanar(config.ChannelCount, UpdateRate);
            for (int frame = 0; frame < inputFrames; ++frame) {
                int sourceBase = pendingOffset + frame * config.ChannelCount;
                for (int channel = 0; channel < config.ChannelCount; ++channel) {
                    nextPlanar[channel][frame] = pendingPcm[sourceBase + channel];
                }
            }

            long renderPosition = nextInputSample + inputFrames / 2;
            ApplyMetadata(renderPosition);
            float[] rendered = listener.Render();
            int consumed = inputFrames;
            pendingOffset += consumed * config.ChannelCount;
            nextInputSample += consumed;
            CompactPending();

            int firstFrame = (int)Math.Min(discardFrames, consumed);
            discardFrames -= firstFrame;
            int outputFrames = checked((int)Math.Min(consumed - firstFrame,
                maximumOutputFrames - emittedFrames));
            if (outputFrames > 0) {
                float[] copy = new float[outputFrames * Pcm714Buffer.Channels];
                Array.Copy(rendered, firstFrame * Pcm714Buffer.Channels, copy, 0, copy.Length);
                output.Append(copy);
                emittedFrames += outputFrames;
            }
        }
    }

    void ApplyConfiguration() {
        for (int index = 0; index < sources.Count; ++index) {
            TrueHdElement element = config.Elements[index];
            sources[index].LFE = element.Lfe;
            if (element.Kind == TrueHdElementKind.Bed && element.SpeakerLabel >= 0) {
                sources[index].Position = SpeakerPosition(element.SpeakerLabel) * Listener.EnvironmentSize;
            }
        }
    }

    void ApplyMetadata(long samplePosition) {
        for (int index = 0; index < sources.Count; ++index) {
            List<TrueHdMetadataEvent> events = metadata[index];
            int cursor = metadataCursor[index];
            while (cursor < events.Count && events[cursor].TargetSample <= samplePosition) {
                stableState[index] = ObjectState.From(events[cursor]);
                ++cursor;
            }
            metadataCursor[index] = cursor;

            ObjectState state = stableState[index];
            if (cursor < events.Count) {
                TrueHdMetadataEvent next = events[cursor];
                long rampStart = next.TargetSample - next.RampSamples;
                if (next.RampSamples > 0 && samplePosition > rampStart) {
                    float amount = Math.Clamp((samplePosition - rampStart) / (float)next.RampSamples, 0, 1);
                    state = ObjectState.Lerp(state, ObjectState.From(next), amount);
                }
            }

            TrueHdElement element = config.Elements[index];
            if (element.Kind != TrueHdElementKind.Bed || element.SpeakerLabel < 0) {
                sources[index].Position = new Vector3(state.X, state.Height, state.FrontBack) *
                    Listener.EnvironmentSize;
            }
            sources[index].Volume = state.Active ? state.Gain : 0;
            sources[index].Size = state.Size;
            sources[index].LFE = element.Lfe || state.Lfe;
        }
    }

    void CompactPending() {
        if (pendingOffset == 0 || pendingOffset < 64 * 1024 && pendingOffset != pendingPcm.Count) return;
        pendingPcm.RemoveRange(0, pendingOffset);
        pendingOffset = 0;
    }

    static float[][] CreatePlanar(int channels, int frames) =>
        Enumerable.Range(0, channels).Select(_ => new float[frames]).ToArray();

    static Vector3 SpeakerPosition(int label) => label switch {
        0 => new(-1, 0, 1), 1 => new(1, 0, 1), 2 => new(0, 0, 1),
        3 => new(-1, -1, 1), 4 => new(-1, 0, 0), 5 => new(1, 0, 0),
        6 => new(-1, 0, -1), 7 => new(1, 0, -1),
        8 => new(-1, 1, 1), 9 => new(1, 1, 1),
        10 => new(-1, 1, 0), 11 => new(1, 1, 0),
        12 => new(-1, 1, -1), 13 => new(1, 1, -1),
        14 => new(-1, 0, .677419f), 15 => new(1, 0, .677419f),
        16 => new(1, -1, 1),
        _ => new(0, 0, 1),
    };

    public void Dispose() => listener.DetachAllSources();

    readonly record struct ObjectState(float X, float FrontBack, float Height, float Gain,
                                       float Size, bool Active, bool Lfe) {
        public static ObjectState Default => new(0, 1, 0, 1, 0, true, false);

        public static ObjectState From(TrueHdMetadataEvent value) => new(
            value.X, value.FrontBack, value.Height,
            value.GainDb <= -128 ? 0 : MathF.Pow(10, value.GainDb / 20),
            value.Size, value.Active, value.Lfe);

        public static ObjectState Lerp(ObjectState from, ObjectState to, float amount) => new(
            Mix(from.X, to.X, amount), Mix(from.FrontBack, to.FrontBack, amount),
            Mix(from.Height, to.Height, amount), Mix(from.Gain, to.Gain, amount),
            Mix(from.Size, to.Size, amount), amount < 1 ? from.Active : to.Active,
            from.Lfe || to.Lfe);

        static float Mix(float from, float to, float amount) => from + (to - from) * amount;
    }
}
