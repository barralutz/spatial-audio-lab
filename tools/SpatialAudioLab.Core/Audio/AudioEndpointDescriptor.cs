namespace SpatialAudioLab.Core.Audio;

public sealed record AudioEndpointDescriptor(
    string Id,
    string Name,
    string? ContainerId,
    bool IsDefault,
    int MaximumChannels48k,
    IReadOnlyList<int> ExclusivePcm48k);
