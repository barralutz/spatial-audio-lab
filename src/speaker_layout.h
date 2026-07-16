#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dolby {

struct SpeakerDefinition {
    std::wstring name;
    double azimuthDegrees{};
    double elevationDegrees{};
    double trimDb{};
};

struct OutputRouteDefinition {
    std::wstring name;
    std::wstring endpointFilter;
    std::vector<std::size_t> speakerIndices;
    double delayMilliseconds{};
};

struct SpeakerLayout {
    std::wstring name;
    std::vector<SpeakerDefinition> speakers;
    std::vector<OutputRouteDefinition> outputs;
    std::size_t masterOutput{};

    std::optional<std::size_t> FindSpeaker(std::wstring_view speakerName) const;
};

SpeakerLayout LoadSpeakerLayout(const std::filesystem::path& path);
std::vector<double> PanDirection(const SpeakerLayout& layout,
                                 double azimuthDegrees,
                                 double height);
std::vector<double> PanMatObject(const SpeakerLayout& layout,
                                 const std::array<unsigned, 6>& fields);

} // namespace dolby
