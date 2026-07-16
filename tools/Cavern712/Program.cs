using System;
using System.IO;

using Cavern;
using Cavern.Channels;
using Cavern.Format;
using Cavern.Format.Common;
using Cavern.Format.Renderers;
using Cavern.Format.Utilities;
using Cavern.Utilities;

const int outputChannels = 10;
const int updateRate = 1536;

if (args.Length < 2 || (args[0] != "inspect" && args[0] != "render")) {
    PrintUsage();
    return 64;
}

string command = args[0];
string input = Path.GetFullPath(args[1]);
if (!File.Exists(input)) {
    Console.Error.WriteLine($"Input file not found: {input}");
    return 66;
}

using AudioReader reader = AudioReader.Open(input);
reader.ReadHeader();
using Renderer renderer = reader.GetRenderer();
bool directTrueHd = reader is AudioTrackReader trackReader && trackReader.track.Format == Codec.TrueHD;

Console.WriteLine($"Input:       {input}");
Console.WriteLine($"Format:      {reader.GetType().Name}");
Console.WriteLine($"Sample rate: {reader.SampleRate} Hz");
Console.WriteLine($"Core:        {reader.ChannelCount} channels");
Console.WriteLine($"Objects:     {renderer.Objects.Count}");
Console.WriteLine($"HasObjects:  {renderer.HasObjects}");
if (directTrueHd) {
    Console.WriteLine("DirectRender: unsupported (decode presentation 3 with truehdd first)");
}

if (command == "inspect" && reader is IMetadataSupplier supplier) {
    ReadableMetadata metadata = supplier.GetMetadata();
    if (metadata != null) {
        foreach (ReadableMetadataHeader header in metadata.Headers) {
            Console.WriteLine($"[{header.Name}]");
            foreach (ReadableMetadataField field in header.Fields) {
                Console.WriteLine($"  {field}");
            }
        }
    }
}

if (command == "inspect") {
    return renderer.HasObjects && !directTrueHd ? 0 : 2;
}

if (directTrueHd) {
    Console.Error.WriteLine(
        "Cavern cannot render TrueHD directly. Convert presentation 3 to DAMF with truehdd, " +
        "then render the generated .atmos file.");
    return 3;
}

if (args.Length < 3) {
    PrintUsage();
    return 64;
}

string output = Path.GetFullPath(args[2]);
double requestedSeconds = args.Length >= 4 ?
    double.Parse(args[3], System.Globalization.CultureInfo.InvariantCulture) :
    reader.Length / (double)reader.SampleRate;
if (requestedSeconds <= 0) {
    Console.Error.WriteLine("Duration must be greater than zero.");
    return 64;
}

long outputFrames = Math.Min(reader.Length, (long)Math.Ceiling(requestedSeconds * reader.SampleRate));
Listener.ReplaceChannels(ChannelPrototype.ToLayout(ChannelPrototype.ref712));
Listener listener = new(false) {
    SampleRate = reader.SampleRate,
    UpdateRate = updateRate,
    AudioQuality = QualityModes.Perfect,
    Volume = renderer.HasObjects ? .707f : 1,
};
listener.AttachSources(renderer.Objects);

Directory.CreateDirectory(Path.GetDirectoryName(output) ?? ".");
using RIFFWaveWriter writer = new(
    output, ChannelPrototype.ref712, outputFrames, reader.SampleRate, BitDepth.Int16);
writer.WriteHeader();

double[] sumSquares = new double[outputChannels];
float[] peaks = new float[outputChannels];
long renderedFrames = 0;
while (renderedFrames < outputFrames) {
    float[] block = listener.Render();
    int frames = (int)Math.Min(updateRate, outputFrames - renderedFrames);
    int samples = frames * outputChannels;
    writer.WriteBlock(block, 0, samples);

    for (int frame = 0; frame < frames; ++frame) {
        int offset = frame * outputChannels;
        for (int channel = 0; channel < outputChannels; ++channel) {
            float sample = block[offset + channel];
            sumSquares[channel] += sample * sample;
            peaks[channel] = Math.Max(peaks[channel], Math.Abs(sample));
        }
    }
    renderedFrames += frames;
}

string[] names = { "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR", "TFL", "TFR" };
Console.WriteLine($"Output:      {output}");
Console.WriteLine($"Rendered:    {renderedFrames / (double)reader.SampleRate:F3} s, PCM16, 7.1.2");
Console.WriteLine("Channel RMS / peak:");
for (int channel = 0; channel < outputChannels; ++channel) {
    double rms = Math.Sqrt(sumSquares[channel] / renderedFrames);
    Console.WriteLine($"  {names[channel],3}: {ToDb(rms),7:F2} dBFS / {ToDb(peaks[channel]),7:F2} dBFS");
}
return 0;

static double ToDb(double value) => value <= 0 ? double.NegativeInfinity : 20 * Math.Log10(value);

static void PrintUsage() {
    Console.Error.WriteLine("Cavern712 inspect <input.eac3|input.mkv|input.atmos>");
    Console.Error.WriteLine("Cavern712 render <input.eac3|input.mkv|input.atmos> <output.wav> [seconds]");
}
