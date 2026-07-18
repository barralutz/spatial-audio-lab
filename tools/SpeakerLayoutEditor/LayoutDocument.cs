using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using SpatialAudioLab.Core.Audio;
using SpatialAudioLab.Core.Profiles;
using CoreOutputRoute = SpatialAudioLab.Core.Profiles.OutputRouteDefinition;
using CoreProfile = SpatialAudioLab.Core.Profiles.ProfileDocument;
using CoreSpeaker = SpatialAudioLab.Core.Profiles.SpeakerDefinition;

namespace SpeakerLayoutEditor;

internal sealed class SpeakerDefinition : INotifyPropertyChanged
{
    private string name = string.Empty;
    private double azimuth;
    private double elevation;
    private double trimDb;

    public string Name { get => name; set => Set(ref name, value); }

    public double Azimuth { get => azimuth; set => Set(ref azimuth, value); }

    public double Elevation { get => elevation; set => Set(ref elevation, value); }

    public double TrimDb { get => trimDb; set => Set(ref trimDb, value); }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void Set<T>(ref T field, T value, [CallerMemberName] string? property = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return;
        }

        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
    }
}

internal sealed class OutputRouteDefinition : INotifyPropertyChanged
{
    private static readonly IReadOnlyDictionary<int, string[]> StandardChannelNames =
        new Dictionary<int, string[]>
        {
            [1] = ["MONO"],
            [2] = ["FL", "FR"],
            [6] = ["FL", "FR", "FC", "LFE", "SL", "SR"],
            [8] = ["FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"],
            [10] = ["FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR", "TFL", "TFR"],
            [12] =
            [
                "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR",
                "TFL", "TFR", "TBL", "TBR"
            ]
        };

    private string name = string.Empty;
    private string endpointId = string.Empty;
    private string endpointName = string.Empty;
    private string? containerId;
    private int expectedChannels;
    private double delayMilliseconds;

    public string Name { get => name; set => Set(ref name, value); }

    public string EndpointId { get => endpointId; private set => Set(ref endpointId, value); }

    public string EndpointName { get => endpointName; private set => Set(ref endpointName, value); }

    public string? ContainerId { get => containerId; private set => Set(ref containerId, value); }

    public int ExpectedChannels
    {
        get => expectedChannels;
        private set => Set(ref expectedChannels, value);
    }

    public string Endpoint => string.IsNullOrWhiteSpace(EndpointId) ? EndpointName : EndpointId;

    public double DelayMilliseconds
    {
        get => delayMilliseconds;
        set => Set(ref delayMilliseconds, Math.Clamp(value, 0, 500));
    }

    public List<string> Speakers { get; } = [];

    public string SpeakersText => string.Join(", ", Speakers);

    public string ChannelMapText => string.Join(", ", Speakers.Select(
        (speaker, index) => $"{PhysicalChannelName(index)}<-{speaker}"));

    public event PropertyChangedEventHandler? PropertyChanged;

    public void ConfigureEndpoint(
        string id,
        string friendlyName,
        string? stableContainerId,
        int maximumChannels)
    {
        EndpointId = id;
        EndpointName = friendlyName;
        ContainerId = stableContainerId;
        ExpectedChannels = maximumChannels;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Endpoint)));
    }

    public void ConfigureEndpoint(AudioEndpointDescriptor endpoint)
    {
        ConfigureEndpoint(
            endpoint.Id,
            endpoint.Name,
            endpoint.ContainerId,
            endpoint.MaximumChannels48k);
    }

    public string PhysicalChannelName(int index)
    {
        if (index < 0 || index >= Speakers.Count)
        {
            throw new ArgumentOutOfRangeException(nameof(index));
        }

        return StandardChannelNames.TryGetValue(Speakers.Count, out string[]? names)
            ? names[index]
            : $"CH{index + 1}";
    }

    public void NotifySpeakersChanged()
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(SpeakersText)));
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(ChannelMapText)));
    }

    private void Set<T>(ref T field, T value, [CallerMemberName] string? property = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return;
        }

        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
    }
}

internal sealed class LayoutDocument
{
    public Guid Id { get; init; }

    public string Name { get; set; } = string.Empty;

    public string LayoutId { get; init; } = "custom";

    public string MasterOutput { get; set; } = string.Empty;

    public List<SpeakerDefinition> Speakers { get; } = [];

    public List<OutputRouteDefinition> Outputs { get; } = [];

    public static LayoutDocument Load(string path)
    {
        try
        {
            return FromProfile(ProfileIniSerializer.Load(path));
        }
        catch (InvalidDataException error) when (
            error.Message.Contains("[profile]", StringComparison.OrdinalIgnoreCase))
        {
            return FromProfile(ProfileIniSerializer.ImportVersion1(path));
        }
    }

    public static LayoutDocument FromProfile(CoreProfile profile)
    {
        LayoutDocument result = new()
        {
            Id = profile.Id,
            Name = profile.Name,
            LayoutId = profile.LayoutId,
            MasterOutput = profile.MasterOutput
        };
        result.Speakers.AddRange(profile.Speakers.Select(speaker => new SpeakerDefinition
        {
            Name = speaker.Name,
            Azimuth = speaker.Azimuth,
            Elevation = speaker.Elevation,
            TrimDb = speaker.TrimDb
        }));
        foreach (CoreOutputRoute route in profile.Outputs)
        {
            OutputRouteDefinition output = new()
            {
                Name = route.Name,
                DelayMilliseconds = route.DelayMilliseconds
            };
            output.ConfigureEndpoint(
                route.Endpoint.Id,
                route.Endpoint.FriendlyName,
                route.Endpoint.ContainerId,
                route.Endpoint.ExpectedChannels);
            output.Speakers.AddRange(route.Speakers);
            result.Outputs.Add(output);
        }

        return result;
    }

    public CoreProfile ToProfile()
    {
        return new CoreProfile
        {
            Id = Id,
            Name = Name,
            LayoutId = LayoutId,
            MasterOutput = MasterOutput,
            Speakers = Speakers.Select(speaker => new CoreSpeaker(
                speaker.Name,
                speaker.Azimuth,
                speaker.Elevation,
                speaker.TrimDb)).ToList(),
            Outputs = Outputs.Select(output => new CoreOutputRoute(
                output.Name,
                new EndpointIdentity(
                    output.EndpointId,
                    output.EndpointName,
                    output.ContainerId,
                    output.ExpectedChannels),
                output.Speakers.ToList(),
                output.DelayMilliseconds)).ToList()
        };
    }

    public void Save(string path) => ProfileIniSerializer.Save(path, ToProfile());

    public OutputRouteDefinition? OutputFor(string speakerName) =>
        Outputs.FirstOrDefault(output => output.Speakers.Contains(
            speakerName,
            StringComparer.OrdinalIgnoreCase));

    public (OutputRouteDefinition Output, int ChannelIndex)? SlotFor(string speakerName)
    {
        foreach (OutputRouteDefinition output in Outputs)
        {
            int index = output.Speakers.FindIndex(name => name.Equals(
                speakerName,
                StringComparison.OrdinalIgnoreCase));
            if (index >= 0)
            {
                return (output, index);
            }
        }

        return null;
    }

    public string AssignSpeakerToSlot(
        string speakerName,
        OutputRouteDefinition target,
        int targetChannelIndex)
    {
        var source = SlotFor(speakerName) ??
            throw new InvalidDataException($"El parlante {speakerName} no tiene una salida.");
        if (targetChannelIndex < 0 || targetChannelIndex >= target.Speakers.Count)
        {
            throw new ArgumentOutOfRangeException(nameof(targetChannelIndex));
        }

        string displacedSpeaker = target.Speakers[targetChannelIndex];
        if (ReferenceEquals(source.Output, target) && source.ChannelIndex == targetChannelIndex)
        {
            return displacedSpeaker;
        }

        source.Output.Speakers[source.ChannelIndex] = displacedSpeaker;
        target.Speakers[targetChannelIndex] = speakerName;
        source.Output.NotifySpeakersChanged();
        if (!ReferenceEquals(source.Output, target))
        {
            target.NotifySpeakersChanged();
        }

        return displacedSpeaker;
    }
}
