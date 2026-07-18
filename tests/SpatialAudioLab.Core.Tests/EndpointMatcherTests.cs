using SpatialAudioLab.Core.Audio;
using SpatialAudioLab.Core.Profiles;
using Xunit;

namespace SpatialAudioLab.Core.Tests;

public sealed class EndpointMatcherTests
{
    [Fact]
    public void Exact_endpoint_id_wins_when_friendly_name_changed()
    {
        EndpointIdentity stored = new("endpoint-1", "Old Speakers", "container-1", 6);
        AudioEndpointDescriptor exact = Endpoint(
            "endpoint-1", "Renamed Speakers", "container-2", 2);
        AudioEndpointDescriptor fallback = Endpoint(
            "endpoint-2", "Old Speakers", "container-1", 8);

        EndpointMatchResult result = EndpointMatcher.Match(stored, [fallback, exact]);

        Assert.Equal(EndpointMatchStatus.Exact, result.Status);
        Assert.Same(exact, result.Endpoint);
    }

    [Fact]
    public void Unique_container_and_capability_match_is_a_fallback()
    {
        EndpointIdentity stored = new("missing", "Old Speakers", "container-1", 6);
        AudioEndpointDescriptor candidate = Endpoint(
            "endpoint-2", "Renamed Speakers", "container-1", 8);

        EndpointMatchResult result = EndpointMatcher.Match(stored, [candidate]);

        Assert.Equal(EndpointMatchStatus.Fallback, result.Status);
        Assert.Same(candidate, result.Endpoint);
    }

    [Fact]
    public void Unique_name_and_capability_match_is_a_fallback()
    {
        EndpointIdentity stored = new("missing", "Speakers", null, 6);
        AudioEndpointDescriptor candidate = Endpoint(
            "endpoint-2", "speakers", null, 6);

        EndpointMatchResult result = EndpointMatcher.Match(stored, [candidate]);

        Assert.Equal(EndpointMatchStatus.Fallback, result.Status);
        Assert.Same(candidate, result.Endpoint);
    }

    [Fact]
    public void Ambiguous_duplicate_names_do_not_return_an_endpoint()
    {
        EndpointIdentity stored = new("missing", "Speakers", null, 2);
        AudioEndpointDescriptor first = Endpoint("endpoint-1", "Speakers", null, 2);
        AudioEndpointDescriptor second = Endpoint("endpoint-2", "speakers", null, 8);

        EndpointMatchResult result = EndpointMatcher.Match(stored, [first, second]);

        Assert.Equal(EndpointMatchStatus.Ambiguous, result.Status);
        Assert.Null(result.Endpoint);
    }

    [Fact]
    public void Channel_incompatible_fallback_is_missing()
    {
        EndpointIdentity stored = new("missing", "Speakers", "container-1", 6);
        AudioEndpointDescriptor candidate = Endpoint(
            "endpoint-2", "Speakers", "container-1", 2);

        EndpointMatchResult result = EndpointMatcher.Match(stored, [candidate]);

        Assert.Equal(EndpointMatchStatus.Missing, result.Status);
        Assert.Null(result.Endpoint);
    }

    [Fact]
    public void Container_match_outranks_friendly_name_match()
    {
        EndpointIdentity stored = new("missing", "Speakers", "container-1", 2);
        AudioEndpointDescriptor byName = Endpoint(
            "endpoint-1", "Speakers", "other-container", 8);
        AudioEndpointDescriptor byContainer = Endpoint(
            "endpoint-2", "Renamed", "container-1", 2);

        EndpointMatchResult result = EndpointMatcher.Match(
            stored,
            [byName, byContainer]);

        Assert.Equal(EndpointMatchStatus.Fallback, result.Status);
        Assert.Same(byContainer, result.Endpoint);
    }

    private static AudioEndpointDescriptor Endpoint(
        string id,
        string name,
        string? containerId,
        int maximumChannels)
    {
        return new AudioEndpointDescriptor(
            id,
            name,
            containerId,
            false,
            maximumChannels,
            maximumChannels == 0 ? [] : [maximumChannels]);
    }
}
