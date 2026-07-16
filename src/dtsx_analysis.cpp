#include "commands.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace dolby {
namespace {

constexpr std::array<std::uint8_t, 4> kExssSync = {0x64, 0x58, 0x20, 0x25};
constexpr std::array<std::uint8_t, 4> kDtsXConfigSync = {0x3a, 0x42, 0x9b, 0x0a};
constexpr std::array<std::uint8_t, 4> kDtsXLosslessSync = {0x75, 0x9a, 0x19, 0x08};
constexpr std::array<std::uint32_t, 16> kDtsSampleRates = {
    8'000, 16'000, 32'000, 64'000, 128'000, 22'050, 44'100, 88'200,
    176'400, 352'800, 12'000, 24'000, 48'000, 96'000, 192'000, 384'000,
};

class BitReader {
public:
    BitReader(const std::uint8_t* data, const std::size_t bytes)
        : data_(data), bits_(bytes * 8) {}

    std::uint32_t Read(const unsigned count) {
        if (count > 32 || position_ + count > bits_) {
            throw std::runtime_error("Truncated DTS-HD EXSS bit field");
        }
        std::uint32_t value = 0;
        for (unsigned index = 0; index < count; ++index, ++position_) {
            value = (value << 1) |
                    ((data_[position_ / 8] >> (7 - (position_ % 8))) & 1U);
        }
        return value;
    }

    void Skip(const std::size_t count) {
        if (position_ + count > bits_) {
            throw std::runtime_error("Truncated DTS-HD EXSS bit field");
        }
        position_ += count;
    }

    std::size_t Position() const { return position_; }

private:
    const std::uint8_t* data_{};
    std::size_t bits_{};
    std::size_t position_{};
};

struct ExssFrameInfo {
    std::size_t frameSize{};
    std::size_t headerSize{};
    std::size_t assetSize{};
    std::size_t privateOffset{};
    std::uint32_t channels{};
    std::uint32_t bitsPerSample{};
    std::uint32_t sampleRate{};
    std::uint32_t representationType{};
    std::uint32_t codingMode{};
    std::uint32_t auxiliarySize{};
    std::uint32_t auxiliaryCodecId{};
    bool staticFields{};
};

struct PrivatePacketInfo {
    std::size_t configOffset{};
    std::size_t configSize{};
    std::size_t losslessOffset{};
    std::size_t losslessSize{};
    bool exactCoverage{};
};

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Could not open DTS-HD elementary stream");
    const std::streamoff length = input.tellg();
    if (length <= 0) throw std::runtime_error("DTS-HD elementary stream is empty");
    if (static_cast<std::uintmax_t>(length) >
        static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("DTS-HD elementary stream is too large");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) throw std::runtime_error("Could not read DTS-HD elementary stream");
    return bytes;
}

bool HasSync(const std::uint8_t* data, const std::size_t bytes,
             const std::size_t offset, const std::array<std::uint8_t, 4>& sync) {
    return offset + sync.size() <= bytes &&
           std::equal(sync.begin(), sync.end(), data + offset);
}

std::size_t FindSync(const std::vector<std::uint8_t>& bytes, const std::size_t start,
                     const std::array<std::uint8_t, 4>& sync) {
    const auto match = std::search(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                   bytes.end(), sync.begin(), sync.end());
    return match == bytes.end()
               ? std::string::npos
               : static_cast<std::size_t>(std::distance(bytes.begin(), match));
}

ExssFrameInfo ParseExssFrame(const std::uint8_t* data, const std::size_t available) {
    if (!HasSync(data, available, 0, kExssSync)) {
        throw std::runtime_error("DTS-HD EXSS sync is missing");
    }

    BitReader bits(data, available);
    bits.Skip(32); // EXSS sync.
    bits.Skip(8);  // User-defined bits.
    const std::uint32_t exssIndex = bits.Read(2);
    const bool wideHeader = bits.Read(1) != 0;
    ExssFrameInfo result;
    result.headerSize = bits.Read(8 + 4 * static_cast<unsigned>(wideHeader)) + 1;
    const unsigned sizeBits = 16 + 4 * static_cast<unsigned>(wideHeader);
    result.frameSize = bits.Read(sizeBits) + 1;
    if (result.headerSize > result.frameSize || result.frameSize > available) {
        throw std::runtime_error("Invalid DTS-HD EXSS frame or header size");
    }

    result.staticFields = bits.Read(1) != 0;
    std::uint32_t presentations = 1;
    std::uint32_t assets = 1;
    bool mixingMetadataEnabled = false;
    if (result.staticFields) {
        bits.Skip(2); // Reference clock.
        bits.Skip(3); // Frame duration.
        if (bits.Read(1) != 0) bits.Skip(36);
        presentations = bits.Read(3) + 1;
        assets = bits.Read(3) + 1;
        if (presentations != 1 || assets != 1) {
            throw std::runtime_error("Multiple EXSS presentations or assets are unsupported");
        }

        std::vector<std::uint32_t> activeMasks(presentations);
        for (auto& mask : activeMasks) mask = bits.Read(exssIndex + 1);
        for (const std::uint32_t mask : activeMasks) {
            bits.Skip(std::popcount(mask) * 8U);
        }
        mixingMetadataEnabled = bits.Read(1) != 0;
        if (mixingMetadataEnabled) {
            throw std::runtime_error("EXSS stream-level mixing metadata is unsupported");
        }
    }

    result.assetSize = bits.Read(sizeBits) + 1;
    const std::size_t descriptorStart = bits.Position();
    const std::size_t descriptorSize = bits.Read(9) + 1;
    const std::size_t descriptorEnd = descriptorStart + descriptorSize * 8;
    bits.Skip(3); // Asset identifier.

    bool embeddedStereo = false;
    if (result.staticFields) {
        if (bits.Read(1) != 0) bits.Skip(4);
        if (bits.Read(1) != 0) bits.Skip(24);
        if (bits.Read(1) != 0) bits.Skip((bits.Read(10) + 1) * 8U);
        result.bitsPerSample = bits.Read(5) + 1;
        const std::uint32_t sampleRateCode = bits.Read(4);
        result.sampleRate = kDtsSampleRates[sampleRateCode];
        result.channels = bits.Read(8) + 1;
        const bool oneToOneSpeakerMap = bits.Read(1) != 0;
        if (oneToOneSpeakerMap) {
            throw std::runtime_error("One-to-one EXSS speaker maps are unsupported");
        }
        result.representationType = bits.Read(3);
    }

    const bool dynamicRangePresent = bits.Read(1) != 0;
    if (dynamicRangePresent) bits.Skip(8);
    if (bits.Read(1) != 0) bits.Skip(5); // Dialog normalization.
    if (dynamicRangePresent && embeddedStereo) bits.Skip(8);
    if (mixingMetadataEnabled && bits.Read(1) != 0) {
        throw std::runtime_error("EXSS asset mixing metadata is unsupported");
    }

    result.codingMode = bits.Read(2);
    if (result.codingMode != 3) {
        throw std::runtime_error("EXSS asset does not use DTS auxiliary coding mode 3");
    }
    result.auxiliarySize = bits.Read(14);
    result.auxiliaryCodecId = bits.Read(8);
    if (bits.Read(1) != 0) bits.Skip(3);
    if (bits.Position() > descriptorEnd) {
        throw std::runtime_error("DTS-HD EXSS asset descriptor overrun");
    }

    result.privateOffset = result.headerSize + result.assetSize;
    if (result.privateOffset > result.frameSize) {
        throw std::runtime_error("DTS-HD EXSS asset exceeds its frame");
    }
    return result;
}

PrivatePacketInfo ParsePrivatePackets(const std::uint8_t* frame,
                                      const ExssFrameInfo& exss) {
    PrivatePacketInfo result;
    const std::size_t bytes = exss.frameSize;
    result.configOffset = exss.privateOffset;
    if (!HasSync(frame, bytes, result.configOffset, kDtsXConfigSync) ||
        result.configOffset + 6 > bytes) {
        return result;
    }

    // Both private packet writers include ten bytes outside their encoded size field.
    result.configSize = 10U + frame[result.configOffset + 5];
    result.losslessOffset = result.configOffset + result.configSize;
    if (!HasSync(frame, bytes, result.losslessOffset, kDtsXLosslessSync) ||
        result.losslessOffset + 8 > bytes) {
        return result;
    }
    const std::size_t encodedSize =
        (static_cast<std::size_t>(frame[result.losslessOffset + 6]) << 8) |
        frame[result.losslessOffset + 7];
    result.losslessSize = 10U + encodedSize;
    result.exactCoverage = result.losslessOffset + result.losslessSize == bytes;
    return result;
}

template <typename Key>
void PrintCounts(const wchar_t* label, const std::map<Key, std::size_t>& counts) {
    std::wcout << L"  " << label << L":";
    for (const auto& [value, count] : counts) {
        std::wcout << L" " << value << L" (" << count << L")";
    }
    std::wcout << L"\n";
}

} // namespace

void AnalyzeDtsXExss(const std::filesystem::path& inputPath) {
    const std::vector<std::uint8_t> bytes = ReadBinaryFile(inputPath);
    std::map<std::size_t, std::size_t> frameSizes;
    std::map<std::size_t, std::size_t> headerSizes;
    std::map<std::size_t, std::size_t> assetSizes;
    std::map<std::size_t, std::size_t> configSizes;
    std::map<std::uint32_t, std::size_t> codingModes;
    std::map<std::uint32_t, std::size_t> auxiliaryIds;
    std::size_t offset = 0;
    std::size_t frames = 0;
    std::size_t malformed = 0;
    std::size_t privateConfigPackets = 0;
    std::size_t privateLosslessPackets = 0;
    std::size_t exactPrivateCoverage = 0;
    std::size_t standardAssetBytes = 0;
    std::size_t privateBytes = 0;
    std::size_t minLosslessSize = std::numeric_limits<std::size_t>::max();
    std::size_t maxLosslessSize = 0;
    ExssFrameInfo representative{};

    while (offset + kExssSync.size() <= bytes.size()) {
        const std::size_t sync = FindSync(bytes, offset, kExssSync);
        if (sync == std::string::npos) break;
        try {
            const ExssFrameInfo exss = ParseExssFrame(bytes.data() + sync, bytes.size() - sync);
            const PrivatePacketInfo packets = ParsePrivatePackets(bytes.data() + sync, exss);
            if (frames == 0) representative = exss;
            ++frames;
            ++frameSizes[exss.frameSize];
            ++headerSizes[exss.headerSize];
            ++assetSizes[exss.assetSize];
            ++codingModes[exss.codingMode];
            ++auxiliaryIds[exss.auxiliaryCodecId];
            standardAssetBytes += exss.assetSize;
            privateBytes += exss.frameSize - exss.privateOffset;
            if (packets.configSize != 0) {
                ++privateConfigPackets;
                ++configSizes[packets.configSize];
            }
            if (packets.losslessSize != 0) {
                ++privateLosslessPackets;
                minLosslessSize = std::min(minLosslessSize, packets.losslessSize);
                maxLosslessSize = std::max(maxLosslessSize, packets.losslessSize);
            }
            if (packets.exactCoverage) ++exactPrivateCoverage;
            offset = sync + exss.frameSize;
        } catch (const std::exception&) {
            ++malformed;
            offset = sync + 1;
        }
    }
    if (frames == 0) throw std::runtime_error("No supported DTS:X EXSS frames were found");

    std::wcout << L"DTS:X EXSS analysis: " << inputPath.wstring() << L"\n"
               << L"Frames=" << frames << L", stream bytes=" << bytes.size()
               << L", malformed candidates=" << malformed << L"\n"
               << L"Standard EXSS descriptor:\n"
               << L"  static=" << (representative.staticFields ? L"yes" : L"no")
               << L", channels=" << representative.channels
               << L", sample-rate=" << representative.sampleRate
               << L", bits=" << representative.bitsPerSample
               << L", representation=" << representative.representationType << L"\n"
               << L"  auxiliary size=" << representative.auxiliarySize
               << L", standard asset bytes=" << standardAssetBytes << L"\n";
    PrintCounts(L"header sizes", headerSizes);
    PrintCounts(L"asset sizes", assetSizes);
    PrintCounts(L"coding modes", codingModes);
    PrintCounts(L"auxiliary codec IDs", auxiliaryIds);

    std::wcout << L"Private DTS:X lossless transport:\n"
               << L"  config sync 0x3A429B0A=" << privateConfigPackets << L"/" << frames
               << L", lossless sync 0x759A1908=" << privateLosslessPackets << L"/" << frames
               << L"\n"
               << L"  exact packet coverage=" << exactPrivateCoverage << L"/" << frames
               << L", private bytes=" << privateBytes << L"\n";
    PrintCounts(L"config packet sizes", configSizes);
    if (privateLosslessPackets != 0) {
        std::wcout << L"  lossless packet size range=" << minLosslessSize << L".."
                   << maxLosslessSize << L" bytes\n";
    }
    std::wcout << L"  EXSS frame size range=" << frameSizes.begin()->first << L".."
               << frameSizes.rbegin()->first << L" bytes (" << frameSizes.size()
               << L" distinct sizes)\n";
    std::wcout << L"  Standard DTS core/XBR/XLL/LBR components: none\n"
               << L"  FFmpeg can parse the EXSS envelope but does not implement private codec ID "
               << representative.auxiliaryCodecId << L".\n";
}

} // namespace dolby
