using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;

namespace SpeakerLayoutEditor;

sealed record BridgeMeterChannel(string Name, float Rms, float Peak);

sealed record BridgeMeterSnapshot(uint Mode,
                                  ulong UpdateCounter,
                                  long PerformanceCounter,
                                  IReadOnlyList<BridgeMeterChannel> Channels) {
    public bool IsFresh => PerformanceCounter > 0 &&
        Math.Abs(Stopwatch.GetTimestamp() - PerformanceCounter) < Stopwatch.Frequency / 2;
}

sealed class BridgeMeterReader : IDisposable {
    const string MappingName = @"Local\DolbyDecoderBridgeMetersV1";
    const uint Magic = 0x544D4244;
    const uint Version = 1;
    const int MaximumChannels = 32;
    const long SequenceOffset = 8;
    const long ChannelCountOffset = 12;
    const long ModeOffset = 16;
    const long UpdateCounterOffset = 24;
    const long PerformanceCounterOffset = 32;
    const long RmsOffset = 40;
    const long PeakOffset = 168;
    const long NamesOffset = 296;
    const int NameCharacters = 16;
    const int NameBytes = NameCharacters * sizeof(char);

    MemoryMappedFile? mapping;
    MemoryMappedViewAccessor? view;

    public bool TryRead(out BridgeMeterSnapshot? snapshot) {
        snapshot = null;
        try {
            EnsureOpen();
            if (view is null) return false;
            for (int attempt = 0; attempt < 3; ++attempt) {
                uint firstSequence = view.ReadUInt32(SequenceOffset);
                if ((firstSequence & 1) != 0) continue;
                Thread.MemoryBarrier();

                if (view.ReadUInt32(0) != Magic || view.ReadUInt32(4) != Version) {
                    CloseMapping();
                    return false;
                }
                uint channelCount = view.ReadUInt32(ChannelCountOffset);
                if (channelCount == 0 || channelCount > MaximumChannels) return false;
                uint mode = view.ReadUInt32(ModeOffset);
                ulong updateCounter = view.ReadUInt64(UpdateCounterOffset);
                long performanceCounter = view.ReadInt64(PerformanceCounterOffset);
                List<BridgeMeterChannel> channels = new((int)channelCount);
                for (int channel = 0; channel < channelCount; ++channel) {
                    channels.Add(new BridgeMeterChannel(
                        ReadName(channel),
                        view.ReadSingle(RmsOffset + channel * sizeof(float)),
                        view.ReadSingle(PeakOffset + channel * sizeof(float))));
                }

                Thread.MemoryBarrier();
                uint secondSequence = view.ReadUInt32(SequenceOffset);
                if (firstSequence == secondSequence && (secondSequence & 1) == 0) {
                    snapshot = new BridgeMeterSnapshot(
                        mode, updateCounter, performanceCounter, channels);
                    return true;
                }
            }
        } catch (FileNotFoundException) {
            CloseMapping();
        } catch (UnauthorizedAccessException) {
            CloseMapping();
        } catch (IOException) {
            CloseMapping();
        }
        return false;
    }

    string ReadName(int channel) {
        StringBuilder name = new(NameCharacters);
        long offset = NamesOffset + channel * NameBytes;
        for (int index = 0; index < NameCharacters; ++index) {
            char character = (char)view!.ReadUInt16(offset + index * sizeof(char));
            if (character == '\0') break;
            name.Append(character);
        }
        return name.ToString();
    }

    void EnsureOpen() {
        if (view is not null) return;
        mapping = MemoryMappedFile.OpenExisting(MappingName, MemoryMappedFileRights.Read);
        view = mapping.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read);
    }

    void CloseMapping() {
        view?.Dispose();
        mapping?.Dispose();
        view = null;
        mapping = null;
    }

    public void Dispose() => CloseMapping();
}

sealed class ChannelMeterControl : Grid {
    const double MinimumDecibels = -60.0;

    readonly Grid track;
    readonly Rectangle rmsFill;
    readonly Rectangle peakMarker;
    readonly TextBlock levelText;
    double rmsDecibels = MinimumDecibels;
    double peakDecibels = MinimumDecibels;
    bool hasSignal;

    public string SpeakerName { get; }

    public ChannelMeterControl(string speakerName) {
        SpeakerName = speakerName;
        Height = 27;
        Margin = new Thickness(0, 0, 12, 3);
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(38) });
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(50) });

        TextBlock label = new() {
            Text = speakerName,
            FontSize = 11,
            FontWeight = FontWeights.SemiBold,
            VerticalAlignment = VerticalAlignment.Center
        };
        Children.Add(label);

        Color meterColor = speakerName.StartsWith('T')
            ? Color.FromRgb(55, 145, 99)
            : Color.FromRgb(52, 120, 165);
        track = new Grid {
            Height = 10,
            Background = new SolidColorBrush(Color.FromRgb(221, 226, 229)),
            ClipToBounds = true,
            VerticalAlignment = VerticalAlignment.Center
        };
        rmsFill = new Rectangle {
            Fill = new SolidColorBrush(meterColor),
            HorizontalAlignment = HorizontalAlignment.Left
        };
        peakMarker = new Rectangle {
            Width = 2,
            Fill = new SolidColorBrush(Color.FromRgb(37, 43, 48)),
            HorizontalAlignment = HorizontalAlignment.Left,
            Visibility = Visibility.Collapsed
        };
        track.Children.Add(rmsFill);
        track.Children.Add(peakMarker);
        SetColumn(track, 1);
        Children.Add(track);

        levelText = new TextBlock {
            Text = "--",
            FontFamily = new FontFamily("Consolas"),
            FontSize = 10,
            TextAlignment = TextAlignment.Right,
            VerticalAlignment = VerticalAlignment.Center
        };
        SetColumn(levelText, 2);
        Children.Add(levelText);
        track.SizeChanged += (_, _) => RenderLevel();
        AutomationProperties.SetName(this, $"Nivel {speakerName}");
    }

    public void SetLevel(float rms, float peak) {
        rmsDecibels = Decibels(rms);
        peakDecibels = Decibels(peak);
        hasSignal = peak > 0.0F;
        levelText.Text = hasSignal && rmsDecibels > MinimumDecibels
            ? rmsDecibels.ToString("0.0")
            : "-inf";
        ToolTip = $"RMS {rmsDecibels:0.0} dBFS | Pico {peakDecibels:0.0} dBFS";
        RenderLevel();
    }

    public void Reset() {
        hasSignal = false;
        rmsDecibels = MinimumDecibels;
        peakDecibels = MinimumDecibels;
        levelText.Text = "--";
        ToolTip = null;
        RenderLevel();
    }

    void RenderLevel() {
        double width = track.ActualWidth;
        rmsFill.Width = width * Normalize(rmsDecibels);
        if (!hasSignal || width <= 0) {
            peakMarker.Visibility = Visibility.Collapsed;
            return;
        }
        peakMarker.Visibility = Visibility.Visible;
        peakMarker.Margin = new Thickness(
            Math.Clamp(width * Normalize(peakDecibels) - 1, 0, Math.Max(0, width - 2)),
            0, 0, 0);
    }

    static double Decibels(float amplitude) =>
        amplitude > 0.0F ? 20.0 * Math.Log10(amplitude) : MinimumDecibels;

    static double Normalize(double decibels) =>
        Math.Clamp((decibels - MinimumDecibels) / -MinimumDecibels, 0.0, 1.0);
}
