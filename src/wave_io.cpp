#include "wave_io.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dolby {

WaveWriter::WaveWriter(const std::filesystem::path& path, const WAVEFORMATEX* format)
    : stream_(path, std::ios::binary), blockAlign_(format->nBlockAlign) {
    if (!stream_) throw std::runtime_error("Could not create capture file");

    const auto formatSize = static_cast<std::uint32_t>(sizeof(WAVEFORMATEX) + format->cbSize);
    stream_.write("RIFF", 4);
    riffSizePosition_ = stream_.tellp();
    WriteUint32(0);
    stream_.write("WAVE", 4);
    stream_.write("fmt ", 4);
    WriteUint32(formatSize);
    stream_.write(reinterpret_cast<const char*>(format), formatSize);
    if ((formatSize & 1U) != 0) stream_.put('\0');
    stream_.write("data", 4);
    dataSizePosition_ = stream_.tellp();
    WriteUint32(0);
}

WaveWriter::~WaveWriter() {
    try {
        Finalize();
    } catch (...) {
    }
}

void WaveWriter::Write(const BYTE* data, const UINT32 frames, const bool silent) {
    const auto bytes = static_cast<std::uint32_t>(frames * blockAlign_);
    if (silent) {
        std::vector<char> zeros(bytes, 0);
        stream_.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    } else {
        stream_.write(reinterpret_cast<const char*>(data), bytes);
    }
    dataBytes_ += bytes;
}

void WaveWriter::Finalize() {
    if (finalized_) return;
    if ((dataBytes_ & 1U) != 0) stream_.put('\0');
    const auto end = stream_.tellp();
    const auto riffBytes = static_cast<std::uint32_t>(end - std::streampos(8));
    stream_.seekp(riffSizePosition_);
    WriteUint32(riffBytes);
    stream_.seekp(dataSizePosition_);
    WriteUint32(dataBytes_);
    stream_.seekp(end);
    stream_.flush();
    finalized_ = true;
}

void WaveWriter::WriteUint32(const std::uint32_t value) {
    stream_.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

std::uint64_t CountIec61937Preambles(const BYTE* data, const std::size_t bytes) {
    constexpr BYTE littleEndian[] = {0x72, 0xf8, 0x1f, 0x4e};
    constexpr BYTE byteSwapped[] = {0xf8, 0x72, 0x4e, 0x1f};
    std::uint64_t count = 0;
    for (std::size_t index = 0; index + 4 <= bytes; ++index) {
        if (std::equal(std::begin(littleEndian), std::end(littleEndian), data + index) ||
            std::equal(std::begin(byteSwapped), std::end(byteSwapped), data + index)) {
            ++count;
        }
    }
    return count;
}

std::uint16_t ReadLittleUint16(const BYTE* data) {
    std::uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::uint32_t ReadLittleUint32(const BYTE* data) {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

WaveImage ReadWaveImage(const std::filesystem::path& inputPath) {
    std::ifstream stream(inputPath, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Could not open WAV file");
    const std::streamsize size = stream.tellg();
    if (size < 12) throw std::runtime_error("Input is too small to be a RIFF/WAVE file");

    WaveImage image;
    image.bytes.resize(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(image.bytes.data()), size);
    if (!stream || std::memcmp(image.bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(image.bytes.data() + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("Input is not a RIFF/WAVE file");
    }

    std::size_t offset = 12;
    while (offset + 8 <= image.bytes.size()) {
        const BYTE* chunk = image.bytes.data() + offset;
        const std::uint32_t chunkBytes = ReadLittleUint32(chunk + 4);
        const std::size_t payload = offset + 8;
        if (payload + chunkBytes > image.bytes.size()) {
            throw std::runtime_error("WAV contains a truncated chunk");
        }
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            image.formatBytes.assign(image.bytes.begin() + payload,
                                     image.bytes.begin() + payload + chunkBytes);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            image.dataOffset = payload;
            image.dataBytes = chunkBytes;
        }
        offset = payload + chunkBytes + (chunkBytes & 1U);
    }
    if (image.formatBytes.size() < sizeof(WAVEFORMATEX) || image.dataBytes == 0) {
        throw std::runtime_error("WAV has no usable fmt/data chunks");
    }
    return image;
}

} // namespace dolby
