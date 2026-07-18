#pragma once

#include "speaker_layout.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace dolby {

using Canonical714Mix = std::vector<std::vector<double>>;

Canonical714Mix BuildCanonical714Mix(const SpeakerLayout& layout);

std::vector<std::int16_t> MixCanonical714Frame(
    const std::array<std::int16_t, 12>& source,
    const Canonical714Mix& mix);

void MixCanonical714Frame(
    const std::array<std::int16_t, 12>& source,
    const Canonical714Mix& mix,
    std::span<std::int16_t> output);

std::vector<float> MixCanonical714Frame(
    const std::array<float, 12>& source,
    const Canonical714Mix& mix);

} // namespace dolby
