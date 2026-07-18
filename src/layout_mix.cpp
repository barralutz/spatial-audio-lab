#include "layout_mix.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace dolby {
namespace {

struct CanonicalSource {
    std::wstring_view name;
    double azimuthDegrees;
    double height;
};

constexpr std::array<CanonicalSource, 12> kCanonical714Sources = {{
    {L"FL", -30.0, 0.0},
    {L"FR", 30.0, 0.0},
    {L"FC", 0.0, 0.0},
    {L"LFE", 0.0, 0.0},
    {L"BL", -150.0, 0.0},
    {L"BR", 150.0, 0.0},
    {L"SL", -90.0, 0.0},
    {L"SR", 90.0, 0.0},
    {L"TFL", -45.0, 1.0},
    {L"TFR", 45.0, 1.0},
    {L"TBL", -135.0, 1.0},
    {L"TBR", 135.0, 1.0},
}};

constexpr double kEqualPower = 0.70710678118654752440;

void NormalizeRow(std::vector<double>& row) {
    double power = 0.0;
    for (const double gain : row) power += gain * gain;
    if (power <= 1.0) return;
    const double scale = 1.0 / std::sqrt(power);
    for (double& gain : row) gain *= scale;
}

std::size_t ValidateMix(const Canonical714Mix& mix) {
    if (mix.size() != kCanonical714Sources.size() || mix.front().empty()) {
        throw std::invalid_argument("Canonical 7.1.4 mix has invalid dimensions");
    }
    const std::size_t destinations = mix.front().size();
    if (std::any_of(mix.begin(), mix.end(), [destinations](const auto& row) {
            return row.size() != destinations;
        })) {
        throw std::invalid_argument("Canonical 7.1.4 mix rows have inconsistent dimensions");
    }
    return destinations;
}

} // namespace

Canonical714Mix BuildCanonical714Mix(const SpeakerLayout& layout) {
    if (layout.speakers.empty()) {
        throw std::invalid_argument("Cannot build a mix for an empty speaker layout");
    }

    Canonical714Mix mix;
    mix.reserve(kCanonical714Sources.size());
    for (const CanonicalSource& source : kCanonical714Sources) {
        std::vector<double> gains(layout.speakers.size());
        const auto exact = layout.FindSpeaker(source.name);
        if (exact.has_value()) {
            gains[*exact] = 1.0;
        } else if (source.name == L"LFE") {
            // LFE is never folded into full-range channels implicitly.
        } else if (source.height > 0.0) {
            const bool left = source.name == L"TFL" || source.name == L"TBL";
            const auto topMiddle = layout.FindSpeaker(left ? L"TML" : L"TMR");
            if (topMiddle.has_value()) {
                gains[*topMiddle] = kEqualPower;
            } else {
                gains = PanDirection(layout, source.azimuthDegrees, source.height);
            }
        } else {
            gains = PanDirection(layout, source.azimuthDegrees, source.height);
        }
        NormalizeRow(gains);
        mix.push_back(std::move(gains));
    }
    return mix;
}

std::vector<std::int16_t> MixCanonical714Frame(
    const std::array<std::int16_t, 12>& source,
    const Canonical714Mix& mix) {
    const std::size_t destinations = ValidateMix(mix);
    std::vector<std::int16_t> output(destinations);
    MixCanonical714Frame(source, mix, output);
    return output;
}

void MixCanonical714Frame(
    const std::array<std::int16_t, 12>& source,
    const Canonical714Mix& mix,
    const std::span<std::int16_t> output) {
    const std::size_t destinations = ValidateMix(mix);
    if (output.size() != destinations) {
        throw std::invalid_argument("Canonical 7.1.4 output buffer has invalid dimensions");
    }
    for (std::size_t destination = 0; destination < destinations; ++destination) {
        double accumulated = 0.0;
        for (std::size_t sourceIndex = 0; sourceIndex < source.size(); ++sourceIndex) {
            accumulated +=
                static_cast<double>(source[sourceIndex]) * mix[sourceIndex][destination];
        }
        const double limited = std::clamp(
            accumulated,
            static_cast<double>(std::numeric_limits<std::int16_t>::min()),
            static_cast<double>(std::numeric_limits<std::int16_t>::max()));
        output[destination] = static_cast<std::int16_t>(std::lround(limited));
    }
}

std::vector<float> MixCanonical714Frame(
    const std::array<float, 12>& source,
    const Canonical714Mix& mix) {
    const std::size_t destinations = ValidateMix(mix);
    std::vector<double> accumulated(destinations);
    for (std::size_t sourceIndex = 0; sourceIndex < source.size(); ++sourceIndex) {
        for (std::size_t destination = 0; destination < destinations; ++destination) {
            accumulated[destination] +=
                static_cast<double>(source[sourceIndex]) * mix[sourceIndex][destination];
        }
    }

    std::vector<float> output(destinations);
    for (std::size_t destination = 0; destination < destinations; ++destination) {
        output[destination] = static_cast<float>(
            std::clamp(accumulated[destination], -1.0, 1.0));
    }
    return output;
}

} // namespace dolby
