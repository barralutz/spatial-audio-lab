#pragma once

#include "audio_platform.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace dolby {

inline constexpr std::array<BYTE, 18> kMatPositionTablePrefix = {
    0x01, 0xd6, 0x1f, 0x37, 0xcd, 0xf3, 0x7c, 0xdf, 0x37,
    0xcd, 0xf3, 0x7c, 0xdf, 0x37, 0xcd, 0xf3, 0x7c, 0xdf};

std::vector<BYTE> UnswapMatTransportWords(const BYTE* data, std::size_t bytes);

template <std::size_t PatternBytes>
std::vector<std::size_t> FindBytePattern(
    const std::vector<BYTE>& data,
    const std::array<BYTE, PatternBytes>& pattern) {
    std::vector<std::size_t> offsets;
    if (data.size() < pattern.size()) return offsets;
    for (std::size_t offset = 0; offset + pattern.size() <= data.size(); ++offset) {
        if (std::equal(pattern.begin(), pattern.end(), data.begin() + offset)) {
            offsets.push_back(offset);
        }
    }
    return offsets;
}

std::array<unsigned, 6> DecodeMatObjectFields(
    const std::vector<BYTE>& payload,
    std::size_t tableOffset,
    std::size_t objectIndex);

} // namespace dolby
