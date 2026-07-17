#pragma once

#include "speaker_layout.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace dolby {

enum class BridgeMeterMode : std::uint32_t {
    Mat = 1,
    DtsX = 2,
    Pcm = 3,
};

class BridgeMeterPublisher {
public:
    BridgeMeterPublisher(const SpeakerLayout& layout, BridgeMeterMode mode);
    ~BridgeMeterPublisher();

    BridgeMeterPublisher(const BridgeMeterPublisher&) = delete;
    BridgeMeterPublisher& operator=(const BridgeMeterPublisher&) = delete;

    void Update(const std::vector<std::int16_t>& samples);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dolby
