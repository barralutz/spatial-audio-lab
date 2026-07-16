#pragma once

#include "audio_platform.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace dolby {

class WaveWriter {
public:
    WaveWriter(const std::filesystem::path& path, const WAVEFORMATEX* format);
    ~WaveWriter();

    WaveWriter(const WaveWriter&) = delete;
    WaveWriter& operator=(const WaveWriter&) = delete;

    void Write(const BYTE* data, UINT32 frames, bool silent);
    void Finalize();

private:
    void WriteUint32(std::uint32_t value);

    std::ofstream stream_;
    std::streampos riffSizePosition_{};
    std::streampos dataSizePosition_{};
    std::uint16_t blockAlign_{};
    std::uint32_t dataBytes_{};
    bool finalized_{};
};

struct WaveImage {
    std::vector<BYTE> bytes;
    std::vector<BYTE> formatBytes;
    std::size_t dataOffset{};
    std::uint32_t dataBytes{};
};

std::uint64_t CountIec61937Preambles(const BYTE* data, std::size_t bytes);
std::uint16_t ReadLittleUint16(const BYTE* data);
std::uint32_t ReadLittleUint32(const BYTE* data);
WaveImage ReadWaveImage(const std::filesystem::path& inputPath);

} // namespace dolby
