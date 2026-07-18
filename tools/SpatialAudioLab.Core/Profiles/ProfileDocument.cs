namespace SpatialAudioLab.Core.Profiles;

public sealed record EndpointIdentity(
    string Id,
    string FriendlyName,
    string? ContainerId,
    int ExpectedChannels);

public sealed record SpeakerDefinition(
    string Name,
    double Azimuth,
    double Elevation,
    double TrimDb);

public sealed record OutputRouteDefinition(
    string Name,
    EndpointIdentity Endpoint,
    IReadOnlyList<string> Speakers,
    double DelayMilliseconds);

public sealed class ProfileDocument
{
    public const int CurrentVersion = 2;

    public int Version { get; init; } = CurrentVersion;

    public required Guid Id { get; init; }

    public required string Name { get; set; }

    public required string LayoutId { get; init; }

    public required string MasterOutput { get; set; }

    public required List<SpeakerDefinition> Speakers { get; init; }

    public required List<OutputRouteDefinition> Outputs { get; init; }

    public void Validate()
    {
        if (Version != CurrentVersion)
        {
            throw new InvalidDataException($"Unsupported profile version: {Version}.");
        }

        if (Id == Guid.Empty)
        {
            throw new InvalidDataException("Profile ID cannot be empty.");
        }

        ValidateValue(Name, "Profile name");
        ValidateValue(LayoutId, "Layout ID");
        ValidateToken(MasterOutput, "Master output");

        if (Speakers.Count is < 2 or > 12)
        {
            throw new InvalidDataException("A profile must contain between 2 and 12 speakers.");
        }

        HashSet<string> speakerNames = new(StringComparer.OrdinalIgnoreCase);
        foreach (SpeakerDefinition speaker in Speakers)
        {
            ValidateToken(speaker.Name, "Speaker name");
            if (!speakerNames.Add(speaker.Name))
            {
                throw new InvalidDataException($"Duplicate speaker name: {speaker.Name}.");
            }

            if (!double.IsFinite(speaker.Azimuth) ||
                !double.IsFinite(speaker.Elevation) ||
                !double.IsFinite(speaker.TrimDb))
            {
                throw new InvalidDataException("Speaker position and trim must be finite.");
            }

            if (speaker.Elevation is < -90 or > 90)
            {
                throw new InvalidDataException("Speaker elevation must be between -90 and 90 degrees.");
            }

            if (speaker.TrimDb is < -60 or > 12)
            {
                throw new InvalidDataException("Speaker trim must be between -60 and 12 dB.");
            }
        }

        if (Outputs.Count == 0)
        {
            throw new InvalidDataException("A profile must contain at least one output route.");
        }

        HashSet<string> outputNames = new(StringComparer.OrdinalIgnoreCase);
        HashSet<string> endpointIds = new(StringComparer.OrdinalIgnoreCase);
        Dictionary<string, int> assignments = speakerNames.ToDictionary(
            name => name,
            _ => 0,
            StringComparer.OrdinalIgnoreCase);

        foreach (OutputRouteDefinition output in Outputs)
        {
            ValidateToken(output.Name, "Output name");
            if (!outputNames.Add(output.Name))
            {
                throw new InvalidDataException($"Duplicate output name: {output.Name}.");
            }

            ValidateValue(output.Endpoint.FriendlyName, "Endpoint name");
            if (!string.IsNullOrWhiteSpace(output.Endpoint.Id) &&
                !endpointIds.Add(output.Endpoint.Id))
            {
                throw new InvalidDataException("Each output route must use a distinct endpoint ID.");
            }

            if (output.Endpoint.ExpectedChannels <= 0)
            {
                throw new InvalidDataException("Expected endpoint channels must be positive.");
            }

            if (output.Speakers.Count == 0)
            {
                throw new InvalidDataException("An output route cannot be empty.");
            }

            if (output.Speakers.Count > output.Endpoint.ExpectedChannels)
            {
                throw new InvalidDataException("An output route exceeds the endpoint channel count.");
            }

            if (!double.IsFinite(output.DelayMilliseconds) ||
                output.DelayMilliseconds is < 0 or > 500)
            {
                throw new InvalidDataException("Output delay must be between 0 and 500 milliseconds.");
            }

            foreach (string speakerName in output.Speakers)
            {
                if (!assignments.TryGetValue(speakerName, out int count))
                {
                    throw new InvalidDataException(
                        $"Output route references unknown speaker: {speakerName}.");
                }

                assignments[speakerName] = count + 1;
            }
        }

        if (!outputNames.Contains(MasterOutput))
        {
            throw new InvalidDataException("Master output does not name an output route.");
        }

        if (assignments.Values.Any(count => count != 1))
        {
            throw new InvalidDataException(
                "Every speaker must be assigned to exactly one output route.");
        }
    }

    private static void ValidateToken(string value, string description)
    {
        ValidateValue(value, description);
        if (value.IndexOfAny([',', '[', ']', '\r', '\n']) >= 0)
        {
            throw new InvalidDataException($"{description} contains an unsupported character.");
        }
    }

    private static void ValidateValue(string value, string description)
    {
        if (string.IsNullOrWhiteSpace(value) || value.IndexOfAny(['\r', '\n']) >= 0)
        {
            throw new InvalidDataException($"{description} cannot be empty or multiline.");
        }
    }
}
