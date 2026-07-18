using SpatialAudioLab.Core.Profiles;

namespace SpatialAudioLab.Core.Audio;

public enum EndpointMatchStatus
{
    Exact,
    Fallback,
    Ambiguous,
    Missing
}

public sealed record EndpointMatchResult(
    EndpointMatchStatus Status,
    AudioEndpointDescriptor? Endpoint);

public static class EndpointMatcher
{
    public static EndpointMatchResult Match(
        EndpointIdentity stored,
        IEnumerable<AudioEndpointDescriptor> endpoints)
    {
        ArgumentNullException.ThrowIfNull(stored);
        ArgumentNullException.ThrowIfNull(endpoints);
        List<AudioEndpointDescriptor> candidates = endpoints.ToList();

        if (!string.IsNullOrWhiteSpace(stored.Id))
        {
            List<AudioEndpointDescriptor> exact = candidates.Where(endpoint =>
                endpoint.Id.Equals(stored.Id, StringComparison.OrdinalIgnoreCase)).ToList();
            if (exact.Count == 1)
            {
                return new EndpointMatchResult(EndpointMatchStatus.Exact, exact[0]);
            }

            if (exact.Count > 1)
            {
                return new EndpointMatchResult(EndpointMatchStatus.Ambiguous, null);
            }
        }

        List<AudioEndpointDescriptor> compatible = candidates.Where(endpoint =>
            endpoint.MaximumChannels48k >= stored.ExpectedChannels).ToList();

        if (!string.IsNullOrWhiteSpace(stored.ContainerId))
        {
            EndpointMatchResult? containerMatch = ResolveFallback(compatible.Where(endpoint =>
                string.Equals(
                    endpoint.ContainerId,
                    stored.ContainerId,
                    StringComparison.OrdinalIgnoreCase)));
            if (containerMatch is not null)
            {
                return containerMatch;
            }
        }

        EndpointMatchResult? nameMatch = ResolveFallback(compatible.Where(endpoint =>
            endpoint.Name.Equals(
                stored.FriendlyName,
                StringComparison.OrdinalIgnoreCase)));
        return nameMatch ?? new EndpointMatchResult(EndpointMatchStatus.Missing, null);
    }

    private static EndpointMatchResult? ResolveFallback(
        IEnumerable<AudioEndpointDescriptor> candidates)
    {
        List<AudioEndpointDescriptor> matches = candidates.Take(2).ToList();
        return matches.Count switch
        {
            0 => null,
            1 => new EndpointMatchResult(EndpointMatchStatus.Fallback, matches[0]),
            _ => new EndpointMatchResult(EndpointMatchStatus.Ambiguous, null)
        };
    }
}
