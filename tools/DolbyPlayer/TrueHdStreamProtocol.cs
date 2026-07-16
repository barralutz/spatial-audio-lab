using System.Buffers.Binary;

namespace DolbyPlayer;

internal enum TrueHdRecordKind : byte {
    Config = 1,
    Pcm = 2,
    Metadata = 3,
    Reconfigure = 4,
    EndOfStream = 255,
}

internal enum TrueHdElementKind : byte { Unknown, Bed, Dynamic, Isf }

internal sealed record TrueHdElement(TrueHdElementKind Kind, int SpeakerLabel, bool Lfe);

internal sealed record TrueHdStreamConfig(int SampleRate, int ChannelCount,
                                          IReadOnlyList<TrueHdElement> Elements);

internal sealed record TrueHdMetadataEvent(int Element, long TargetSample, int RampSamples,
                                           bool Active, bool BedOrIsf, bool Lfe,
                                           float X, float FrontBack, float Height,
                                           float GainDb, float Size);

internal sealed record TrueHdRecord(TrueHdRecordKind Kind, long SamplePosition,
                                    TrueHdStreamConfig? Config = null,
                                    float[]? Pcm = null, int PcmFrames = 0,
                                    IReadOnlyList<TrueHdMetadataEvent>? Metadata = null);

internal sealed class TrueHdStreamProtocol {
    const int StreamHeaderSize = 16;
    const int RecordHeaderSize = 24;
    const int MaximumPayloadSize = 32 * 1024 * 1024;
    readonly Stream input;

    public TrueHdStreamProtocol(Stream input) => this.input = input;

    public async Task ReadHeaderAsync(CancellationToken cancellationToken) {
        byte[] header = new byte[StreamHeaderSize];
        await input.ReadExactlyAsync(header, cancellationToken);
        if (!header.AsSpan(0, 4).SequenceEqual("THDS"u8)) {
            throw new InvalidDataException("Invalid TrueHD streaming protocol signature.");
        }
        ushort version = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(4, 2));
        ushort size = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(6, 2));
        uint flags = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(8, 4));
        if (version != 1 || size < StreamHeaderSize || (flags & 1) == 0) {
            throw new InvalidDataException($"Unsupported THDS header: version={version}, size={size}, flags={flags}.");
        }
        if (size > StreamHeaderSize) await SkipAsync(size - StreamHeaderSize, cancellationToken);
    }

    public async Task<TrueHdRecord> ReadRecordAsync(CancellationToken cancellationToken) {
        byte[] header = new byte[RecordHeaderSize];
        await input.ReadExactlyAsync(header, cancellationToken);
        TrueHdRecordKind kind = (TrueHdRecordKind)header[0];
        ushort headerSize = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(2, 2));
        int payloadSize = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4, 4)));
        long samplePosition = checked((long)BinaryPrimitives.ReadUInt64LittleEndian(header.AsSpan(8, 8)));
        if (headerSize < RecordHeaderSize || payloadSize < 0 || payloadSize > MaximumPayloadSize) {
            throw new InvalidDataException($"Invalid THDS record header: kind={kind}, header={headerSize}, payload={payloadSize}.");
        }
        if (headerSize > RecordHeaderSize) await SkipAsync(headerSize - RecordHeaderSize, cancellationToken);
        byte[] payload = new byte[payloadSize];
        await input.ReadExactlyAsync(payload, cancellationToken);

        return kind switch {
            TrueHdRecordKind.Config or TrueHdRecordKind.Reconfigure =>
                new TrueHdRecord(kind, samplePosition, Config: ParseConfig(payload)),
            TrueHdRecordKind.Pcm => ParsePcm(samplePosition, payload),
            TrueHdRecordKind.Metadata =>
                new TrueHdRecord(kind, samplePosition, Metadata: ParseMetadata(payload)),
            TrueHdRecordKind.EndOfStream when payloadSize == 0 => new TrueHdRecord(kind, samplePosition),
            _ => new TrueHdRecord(kind, samplePosition),
        };
    }

    static TrueHdStreamConfig ParseConfig(ReadOnlySpan<byte> payload) {
        if (payload.Length < 20) throw new InvalidDataException("Truncated THDS CONFIG record.");
        int sampleRate = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(payload));
        int channels = BinaryPrimitives.ReadUInt16LittleEndian(payload[4..]);
        int sampleFormat = BinaryPrimitives.ReadUInt16LittleEndian(payload[6..]);
        int descriptors = BinaryPrimitives.ReadUInt16LittleEndian(payload[18..]);
        if (sampleFormat != 1 || channels <= 0 || descriptors != channels || payload.Length != 20 + descriptors * 8) {
            throw new InvalidDataException(
                $"Unsupported THDS configuration: {channels} channels, format {sampleFormat}, {descriptors} descriptors.");
        }
        List<TrueHdElement> elements = new(channels);
        for (int index = 0; index < descriptors; ++index) {
            ReadOnlySpan<byte> descriptor = payload.Slice(20 + index * 8, 8);
            elements.Add(new TrueHdElement((TrueHdElementKind)descriptor[0], (sbyte)descriptor[1],
                (BinaryPrimitives.ReadUInt16LittleEndian(descriptor[2..]) & 1) != 0));
        }
        return new TrueHdStreamConfig(sampleRate, channels, elements);
    }

    static TrueHdRecord ParsePcm(long position, ReadOnlySpan<byte> payload) {
        if (payload.Length < 4) throw new InvalidDataException("Truncated THDS PCM record.");
        int frames = BinaryPrimitives.ReadUInt16LittleEndian(payload);
        int channels = BinaryPrimitives.ReadUInt16LittleEndian(payload[2..]);
        int samples = checked(frames * channels);
        if (channels <= 0 || payload.Length != 4 + samples * sizeof(int)) {
            throw new InvalidDataException("Invalid THDS PCM payload size.");
        }
        float[] pcm = new float[samples];
        for (int sample = 0; sample < samples; ++sample) {
            int value = BinaryPrimitives.ReadInt32LittleEndian(payload.Slice(4 + sample * sizeof(int), sizeof(int)));
            pcm[sample] = value / 8388608f;
        }
        return new TrueHdRecord(TrueHdRecordKind.Pcm, position, Pcm: pcm, PcmFrames: frames);
    }

    static IReadOnlyList<TrueHdMetadataEvent> ParseMetadata(ReadOnlySpan<byte> payload) {
        const int EventSize = 48;
        if (payload.Length < 4) throw new InvalidDataException("Truncated THDS META record.");
        int count = BinaryPrimitives.ReadUInt16LittleEndian(payload);
        int version = BinaryPrimitives.ReadUInt16LittleEndian(payload[2..]);
        if (version != 1 || payload.Length != 4 + count * EventSize) {
            throw new InvalidDataException($"Unsupported THDS metadata version or size: {version}.");
        }
        List<TrueHdMetadataEvent> events = new(count);
        for (int index = 0; index < count; ++index) {
            ReadOnlySpan<byte> item = payload.Slice(4 + index * EventSize, EventSize);
            int flags = BinaryPrimitives.ReadUInt16LittleEndian(item[2..]);
            events.Add(new TrueHdMetadataEvent(
                BinaryPrimitives.ReadUInt16LittleEndian(item),
                checked((long)BinaryPrimitives.ReadUInt64LittleEndian(item[4..])),
                checked((int)BinaryPrimitives.ReadUInt32LittleEndian(item[12..])),
                (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0,
                ReadSingle(item[20..]), ReadSingle(item[24..]), ReadSingle(item[28..]),
                BinaryPrimitives.ReadInt16LittleEndian(item[16..]), ReadSingle(item[32..])));
        }
        return events;
    }

    static float ReadSingle(ReadOnlySpan<byte> source) =>
        BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(source));

    async Task SkipAsync(int bytes, CancellationToken cancellationToken) {
        byte[] scratch = new byte[Math.Min(bytes, 4096)];
        while (bytes > 0) {
            int read = Math.Min(bytes, scratch.Length);
            await input.ReadExactlyAsync(scratch.AsMemory(0, read), cancellationToken);
            bytes -= read;
        }
    }
}
