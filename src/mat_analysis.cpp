#include "commands.h"

#include "audio_platform.h"
#include "mat_format.h"
#include "wave_io.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dolby {

namespace {

std::wstring Iec61937DataTypeName(const std::uint16_t pc) {
    switch (pc & 0x1fU) {
    case 0x01: return L"AC-3";
    case 0x0b: return L"DTS type I";
    case 0x0c: return L"DTS type II";
    case 0x0d: return L"DTS type III";
    case 0x11: return L"DTS-HD type IV";
    case 0x15: return L"E-AC-3";
    case 0x16: return L"Dolby MAT";
    default: return L"unknown";
    }
}

struct Iec61937Header {
    std::size_t offset{};
    std::uint16_t pc{};
    std::uint16_t pd{};
    bool byteSwapped{};
};

std::vector<Iec61937Header> FindIec61937Headers(const BYTE* data,
                                                const std::size_t dataBytes) {
    std::vector<Iec61937Header> headers;
    for (std::size_t offset = 0; offset + 8 <= dataBytes; ++offset) {
        const bool littleEndian = data[offset] == 0x72 && data[offset + 1] == 0xf8 &&
                                  data[offset + 2] == 0x1f && data[offset + 3] == 0x4e;
        const bool byteSwapped = data[offset] == 0xf8 && data[offset + 1] == 0x72 &&
                                 data[offset + 2] == 0x4e && data[offset + 3] == 0x1f;
        if (!littleEndian && !byteSwapped) continue;
        const auto readWord = [&](const std::size_t wordOffset) {
            return byteSwapped
                       ? static_cast<std::uint16_t>((data[wordOffset] << 8) |
                                                    data[wordOffset + 1])
                       : ReadLittleUint16(data + wordOffset);
        };
        headers.push_back({offset, readWord(offset + 4), readWord(offset + 6), byteSwapped});
    }
    return headers;
}

std::size_t Iec61937PayloadBytes(const std::uint16_t pc, const std::uint16_t pd) {
    switch (pc & 0x1fU) {
    case 0x11: // DTS-HD type IV
    case 0x15: // E-AC-3
    case 0x16: // Dolby MAT
        return pd;
    default:
        return (static_cast<std::uint32_t>(pd) + 7) / 8;
    }
}

} // namespace

std::wstring SpeakerName(const DWORD speaker) {
    switch (speaker) {
    case SPEAKER_FRONT_LEFT: return L"FL";
    case SPEAKER_FRONT_RIGHT: return L"FR";
    case SPEAKER_FRONT_CENTER: return L"FC";
    case SPEAKER_LOW_FREQUENCY: return L"LFE";
    case SPEAKER_BACK_LEFT: return L"BL";
    case SPEAKER_BACK_RIGHT: return L"BR";
    case SPEAKER_FRONT_LEFT_OF_CENTER: return L"FLC";
    case SPEAKER_FRONT_RIGHT_OF_CENTER: return L"FRC";
    case SPEAKER_BACK_CENTER: return L"BC";
    case SPEAKER_SIDE_LEFT: return L"SL";
    case SPEAKER_SIDE_RIGHT: return L"SR";
    case SPEAKER_TOP_CENTER: return L"TC";
    case SPEAKER_TOP_FRONT_LEFT: return L"TFL";
    case SPEAKER_TOP_FRONT_CENTER: return L"TFC";
    case SPEAKER_TOP_FRONT_RIGHT: return L"TFR";
    case SPEAKER_TOP_BACK_LEFT: return L"TBL";
    case SPEAKER_TOP_BACK_CENTER: return L"TBC";
    case SPEAKER_TOP_BACK_RIGHT: return L"TBR";
    default: return L"0x" + std::to_wstring(speaker);
    }
}

std::vector<std::wstring> ChannelNames(const DWORD mask, const WORD channels) {
    std::vector<std::wstring> names;
    for (DWORD bit = 1; bit != 0 && names.size() < channels; bit <<= 1) {
        if ((mask & bit) != 0) names.push_back(SpeakerName(bit));
    }
    while (names.size() < channels) names.push_back(L"CH" + std::to_wstring(names.size() + 1));
    return names;
}

void AnalyzeIec61937Wave(const std::filesystem::path& inputPath) {
    const WaveImage image = ReadWaveImage(inputPath);
    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(image.formatBytes.data());
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        image.formatBytes.size() < sizeof(WAVEFORMATEXTENSIBLE)) {
        throw std::runtime_error("IEC 61937 analyzer requires WAVEFORMATEXTENSIBLE");
    }

    const BYTE* data = image.bytes.data() + image.dataOffset;
    const std::size_t dataBytes = image.dataBytes;
    const std::vector<Iec61937Header> headers = FindIec61937Headers(data, dataBytes);
    if (headers.empty()) throw std::runtime_error("No IEC 61937 preambles were found");

    std::map<std::uint16_t, std::size_t> typeCounts;
    std::map<std::size_t, std::size_t> spacingCounts;
    for (const Iec61937Header& header : headers) ++typeCounts[header.pc];
    for (std::size_t index = 1; index < headers.size(); ++index) {
        ++spacingCounts[headers[index].offset - headers[index - 1].offset];
    }

    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    std::wcout << L"File: " << inputPath.wstring() << L"\n"
               << L"Format: " << WaveFormatText(format) << L"\n"
               << L"Data: " << dataBytes << L" bytes, " << std::fixed << std::setprecision(3)
               << (static_cast<double>(dataBytes) / format->nAvgBytesPerSec) << L" s\n"
               << L"Subtype: " << WaveFormatText(&extensible->Format) << L"\n"
               << L"IEC preambles: " << headers.size() << L", first offset: "
               << headers.front().offset << L" bytes\n";

    std::wcout << L"Data types:\n";
    for (const auto& [pc, count] : typeCounts) {
        std::wcout << L"  Pc=0x" << std::hex << std::uppercase << pc << std::dec << L" ("
                   << Iec61937DataTypeName(pc) << L"): " << count << L" bursts\n";
    }
    if (!spacingCounts.empty()) {
        const auto commonSpacing = std::max_element(
            spacingCounts.begin(), spacingCounts.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; });
        std::wcout << L"Most common spacing: " << commonSpacing->first << L" bytes ("
                   << commonSpacing->second << L" intervals)\n";
    }

    std::wcout << L"First IEC headers:\n";
    for (std::size_t index = 0; index < std::min<std::size_t>(10, headers.size()); ++index) {
        const Iec61937Header& header = headers[index];
        std::wcout << L"  " << index << L": offset=" << header.offset << L", Pc=0x"
                   << std::hex << std::uppercase << header.pc << L", Pd=0x" << header.pd
                   << std::dec << L" (" << Iec61937PayloadBytes(header.pc, header.pd)
                   << L" payload bytes), "
                   << (header.byteSwapped ? L"byte-swapped" : L"little-endian") << L"\n";
    }
}

void ExtractDtsHdWave(const std::filesystem::path& inputPath,
                      const std::filesystem::path& outputPath) {
    const WaveImage image = ReadWaveImage(inputPath);
    const BYTE* data = image.bytes.data() + image.dataOffset;
    const std::size_t dataBytes = image.dataBytes;
    const std::vector<Iec61937Header> headers = FindIec61937Headers(data, dataBytes);
    if (headers.empty()) throw std::runtime_error("No IEC 61937 preambles were found");

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Could not create DTS-HD output file");

    constexpr std::array<BYTE, 10> startCode = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xfe};
    std::size_t frames = 0;
    std::size_t frameBytesWritten = 0;
    std::size_t malformed = 0;
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const Iec61937Header& header = headers[index];
        if ((header.pc & 0x1fU) != 0x11) {
            throw std::runtime_error("IEC 61937 stream is not DTS-HD type IV");
        }
        const std::size_t burstEnd = index + 1 < headers.size()
                                         ? headers[index + 1].offset
                                         : dataBytes;
        const std::size_t available = burstEnd > header.offset + 8
                                          ? burstEnd - header.offset - 8
                                          : 0;
        const std::size_t payloadBytes = Iec61937PayloadBytes(header.pc, header.pd);
        if (payloadBytes < 12 || payloadBytes > available) {
            ++malformed;
            continue;
        }
        const std::vector<BYTE> logical =
            UnswapMatTransportWords(data + header.offset + 8, payloadBytes);
        if (!std::equal(startCode.begin(), startCode.end(), logical.begin())) {
            ++malformed;
            continue;
        }
        const std::size_t frameBytes =
            (static_cast<std::size_t>(logical[10]) << 8) | logical[11];
        if (frameBytes == 0 || frameBytes + 12 > logical.size()) {
            ++malformed;
            continue;
        }
        output.write(reinterpret_cast<const char*>(logical.data() + 12),
                     static_cast<std::streamsize>(frameBytes));
        if (!output) throw std::runtime_error("Failed while writing DTS-HD output");
        ++frames;
        frameBytesWritten += frameBytes;
    }
    output.close();
    if (frames == 0) throw std::runtime_error("No valid DTS-HD frames were extracted");

    std::wcout << L"DTS-HD extraction written: " << outputPath.wstring() << L"\n"
               << L"Frames=" << frames << L", bytes=" << frameBytesWritten
               << L", malformed bursts=" << malformed << L"\n";
}

double GoertzelAmplitude(const BYTE* data, const std::size_t frames, const WORD blockAlign,
                         const WORD lane, const DWORD sampleRate, const double frequency) {
    const double omega = 2.0 * 3.14159265358979323846 * frequency / sampleRate;
    const double coefficient = 2.0 * std::cos(omega);
    double first = 0.0;
    double second = 0.0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const BYTE* sample = data + frame * blockAlign + lane * sizeof(std::int16_t);
        const double input = static_cast<double>(static_cast<std::int16_t>(ReadLittleUint16(sample))) /
                             32768.0;
        const double next = input + coefficient * first - second;
        second = first;
        first = next;
    }
    const double power = first * first + second * second - coefficient * first * second;
    return frames == 0 ? 0.0 : 2.0 * std::sqrt(std::max(0.0, power)) / frames;
}

struct PcmWindowMatch {
    std::size_t offset{};
    double amplitude{};
    double rms{};
    double score{};
};

struct PcmLevelAccumulator {
    long double squareSum{};
    long double sum{};
    double peak{};
    std::uint64_t samples{};
    std::size_t activeBlocks{};
};

void AccumulatePcmLevel(PcmLevelAccumulator& level, const BYTE* source,
                        const std::size_t frames, const bool bigEndian) {
    long double blockSquareSum = 0.0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const BYTE* sample = source + frame * sizeof(std::int16_t);
        const std::uint16_t bits = bigEndian
                                       ? static_cast<std::uint16_t>((sample[0] << 8) | sample[1])
                                       : ReadLittleUint16(sample);
        const double normalized =
            static_cast<double>(static_cast<std::int16_t>(bits)) / 32768.0;
        level.sum += normalized;
        level.squareSum += normalized * normalized;
        blockSquareSum += normalized * normalized;
        level.peak = std::max(level.peak, std::abs(normalized));
        ++level.samples;
    }
    const double blockRms = std::sqrt(static_cast<double>(blockSquareSum / frames));
    if (blockRms >= 0.001) ++level.activeBlocks;
}

PcmWindowMatch FindPcmToneWindow(const BYTE* payload, const std::size_t payloadBytes,
                                 const double frequency, const bool bigEndian,
                                 const std::size_t pcmFrames = 480,
                                 const double pcmRate = 48'000.0) {
    const double omega = 2.0 * 3.14159265358979323846 * frequency / pcmRate;
    const double coefficient = 2.0 * std::cos(omega);
    PcmWindowMatch best;

    if (payloadBytes < pcmFrames * sizeof(std::int16_t)) return best;
    for (std::size_t offset = 0;
         offset + pcmFrames * sizeof(std::int16_t) <= payloadBytes; offset += 2) {
        double first = 0.0;
        double second = 0.0;
        long double squareSum = 0.0;
        for (std::size_t frame = 0; frame < pcmFrames; ++frame) {
            const BYTE* sample = payload + offset + frame * sizeof(std::int16_t);
            const std::uint16_t bits = bigEndian
                                           ? static_cast<std::uint16_t>((sample[0] << 8) | sample[1])
                                           : ReadLittleUint16(sample);
            const double input = static_cast<double>(static_cast<std::int16_t>(bits)) / 32768.0;
            squareSum += input * input;
            const double next = input + coefficient * first - second;
            second = first;
            first = next;
        }
        const double power = first * first + second * second - coefficient * first * second;
        const double amplitude = 2.0 * std::sqrt(std::max(0.0, power)) / pcmFrames;
        const double rms = std::sqrt(static_cast<double>(squareSum / pcmFrames));
        const double score = rms > 1.0e-5 ? amplitude / rms : 0.0;
        if (amplitude >= 0.002 && score > best.score) {
            best = {offset, amplitude, rms, score};
        }
    }
    return best;
}

std::vector<std::size_t> FindEmdfBitOffsets(const std::vector<BYTE>& data) {
    const std::size_t totalBits = data.size() * 8;
    const auto readBit = [&data](const std::size_t position) {
        return static_cast<unsigned>((data[position >> 3] >> (7 - (position & 7))) & 1);
    };
    const auto readBits = [&readBit](const std::size_t start, const std::size_t count) {
        std::size_t value = 0;
        for (std::size_t bit = 0; bit < count; ++bit) {
            value = (value << 1) | readBit(start + bit);
        }
        return value;
    };

    std::vector<std::size_t> offsets;
    for (std::size_t start = 0; start + 32 <= totalBits; ++start) {
        if (readBits(start, 16) != 0x5838) continue;
        const std::size_t bytes = readBits(start + 16, 16);
        if (bytes != 0 && start + 32 + bytes * 8 <= totalBits) offsets.push_back(start);
    }
    return offsets;
}

void AnalyzeMatWave(const std::filesystem::path& inputPath) {
    const WaveImage image = ReadWaveImage(inputPath);
    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(image.formatBytes.data());
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        image.formatBytes.size() < sizeof(WAVEFORMATEXTENSIBLE)) {
        throw std::runtime_error("MAT analyzer requires WAVEFORMATEXTENSIBLE");
    }
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    if (!IsEqualGUID(extensible->SubFormat, kDolbyMat20) &&
        !IsEqualGUID(extensible->SubFormat, kDolbyMat21Profile3) &&
        !IsEqualGUID(extensible->SubFormat, kDolbyMlpMat10)) {
        throw std::runtime_error("WAV subtype is not a recognized Dolby MAT carrier");
    }
    if (format->nChannels != 8 || format->wBitsPerSample != 16 ||
        format->nBlockAlign != 16) {
        throw std::runtime_error("MAT analyzer currently requires an 8-channel, 16-bit carrier");
    }

    const BYTE* data = image.bytes.data() + image.dataOffset;
    const std::size_t dataBytes = image.dataBytes;
    std::vector<std::size_t> preambles;
    for (std::size_t offset = 0; offset + 8 <= dataBytes; ++offset) {
        if (data[offset] == 0x72 && data[offset + 1] == 0xf8 &&
            data[offset + 2] == 0x1f && data[offset + 3] == 0x4e) {
            preambles.push_back(offset);
        }
    }
    if (preambles.empty()) throw std::runtime_error("No IEC 61937 preambles were found");

    std::size_t minimumSpacing = std::numeric_limits<std::size_t>::max();
    std::size_t maximumSpacing = 0;
    std::size_t irregularSpacings = 0;
    const std::size_t expectedSpacing =
        static_cast<std::size_t>(format->nAvgBytesPerSec / 50); // MAT uses 20 ms bursts.
    for (std::size_t index = 1; index < preambles.size(); ++index) {
        const std::size_t spacing = preambles[index] - preambles[index - 1];
        minimumSpacing = std::min(minimumSpacing, spacing);
        maximumSpacing = std::max(maximumSpacing, spacing);
        if (spacing != expectedSpacing) ++irregularSpacings;
    }

    std::wcout << L"File: " << inputPath.wstring() << L"\n"
               << L"Format: " << WaveFormatText(format) << L"\n"
               << L"Data: " << dataBytes << L" bytes, "
               << (dataBytes / format->nBlockAlign) << L" carrier frames, "
               << std::fixed << std::setprecision(3)
               << (static_cast<double>(dataBytes) / format->nAvgBytesPerSec) << L" s\n"
               << L"IEC preambles: " << preambles.size()
               << L", first offset: " << preambles.front() << L" bytes\n";
    if (preambles.size() > 1) {
        std::wcout << L"Preamble spacing: min=" << minimumSpacing << L", max=" << maximumSpacing
                   << L", expected=" << expectedSpacing
                   << L", irregular=" << irregularSpacings << L"\n";
    }

    std::wcout << L"First IEC headers (Pc/Pd):\n";
    for (std::size_t index = 0; index < std::min<std::size_t>(5, preambles.size()); ++index) {
        const BYTE* header = data + preambles[index];
        std::wcout << L"  " << index << L": offset=" << preambles[index]
                   << L", Pc=0x" << std::hex << std::uppercase << ReadLittleUint16(header + 4)
                   << L", Pd=0x" << ReadLittleUint16(header + 6) << std::dec << L"\n";
    }

    const std::array<double, 11> probeFrequencies = {
        55.0, 220.0, 277.0, 330.0, 440.0, 554.0, 660.0, 831.0, 900.0, 1000.0, 1500.0};
    const std::size_t frames = dataBytes / format->nBlockAlign;
    std::wcout << L"Strongest raw carrier lane per probe frequency:\n";
    for (const double frequency : probeFrequencies) {
        double bestAmplitude = 0.0;
        WORD bestLane = 0;
        for (WORD lane = 0; lane < format->nChannels; ++lane) {
            const double amplitude = GoertzelAmplitude(data, frames, format->nBlockAlign, lane,
                                                       format->nSamplesPerSec, frequency);
            if (amplitude > bestAmplitude) {
                bestAmplitude = amplitude;
                bestLane = lane;
            }
        }
        std::wcout << L"  " << std::setw(6) << std::setprecision(1) << frequency << L" Hz: lane "
                   << (bestLane + 1) << L", " << std::setprecision(2)
                   << Decibels(bestAmplitude) << L" dBFS\n";
    }

    const std::size_t stableIndex = std::min<std::size_t>(10, preambles.size() - 1);
    const std::size_t stableOffset = preambles[stableIndex] + 8;
    const std::size_t stableBytes = std::min<std::size_t>(
        16'384, dataBytes > stableOffset ? dataBytes - stableOffset : 0);
    constexpr std::size_t secondHalfOffset = 30'708;
    const std::size_t secondHalfBytes = std::min<std::size_t>(
        16'384, dataBytes > stableOffset + secondHalfOffset
                    ? dataBytes - stableOffset - secondHalfOffset
                    : 0);
    std::wcout << L"Best contiguous 48 kHz PCM windows in MAT burst " << stableIndex << L":\n";

    constexpr std::array<BYTE, 6> fullBandMarker = {0x83, 0x41, 0x43, 0xc2, 0x08, 0x2f};
    constexpr std::array<BYTE, 6> lfeMarker = {0x83, 0x41, 0x40, 0xf2, 0x0b, 0x2f};
    const std::size_t carrierPayloadBytes = preambles.size() > stableIndex + 1
                                                ? preambles[stableIndex + 1] - preambles[stableIndex] - 8
                                                : expectedSpacing - 8;
    const auto logicalPayload =
        UnswapMatTransportWords(data + stableOffset, carrierPayloadBytes);
    const auto fullBandMarkers = FindBytePattern(logicalPayload, fullBandMarker);
    const auto lfeMarkers = FindBytePattern(logicalPayload, lfeMarker);
    const auto emdfOffsets = FindEmdfBitOffsets(logicalPayload);
    std::wcout << L"Logical PCM markers: full-band=" << fullBandMarkers.size() << L" [";
    for (std::size_t index = 0; index < fullBandMarkers.size(); ++index) {
        if (index != 0) std::wcout << L", ";
        std::wcout << fullBandMarkers[index];
    }
    std::wcout << L"], LFE=" << lfeMarkers.size() << L" [";
    for (std::size_t index = 0; index < lfeMarkers.size(); ++index) {
        if (index != 0) std::wcout << L", ";
        std::wcout << lfeMarkers[index];
    }
    std::wcout << L"]\n";
    std::wcout << L"Valid EMDF syncs at any bit alignment: " << emdfOffsets.size();
    for (const std::size_t offset : emdfOffsets) std::wcout << L" " << offset;
    std::wcout << L"\n";

    std::map<std::size_t, std::size_t> markerCountHistogram;
    for (std::size_t burst = 0; burst < preambles.size(); ++burst) {
        const std::size_t payloadOffset = preambles[burst] + 8;
        const std::size_t payloadBytes = burst + 1 < preambles.size()
                                             ? preambles[burst + 1] - payloadOffset
                                             : dataBytes - payloadOffset;
        const auto logical = UnswapMatTransportWords(data + payloadOffset, payloadBytes);
        ++markerCountHistogram[FindBytePattern(logical, fullBandMarker).size()];
    }
    const auto commonMarkerCount = std::max_element(
        markerCountHistogram.begin(), markerCountHistogram.end(),
        [](const auto& left, const auto& right) { return left.second < right.second; });
    if (commonMarkerCount != markerCountHistogram.end() &&
        commonMarkerCount->first >= 2 && commonMarkerCount->first % 2 == 0) {
        const std::size_t slotsPerHalf = commonMarkerCount->first / 2;
        std::vector<PcmLevelAccumulator> slotLevels(slotsPerHalf);
        std::array<std::vector<PcmLevelAccumulator>, 2> halfSlotLevels = {
            std::vector<PcmLevelAccumulator>(slotsPerHalf),
            std::vector<PcmLevelAccumulator>(slotsPerHalf)};
        PcmLevelAccumulator lfeLevel;
        std::array<PcmLevelAccumulator, 2> halfLfeLevels{};
        std::size_t analyzedBursts = 0;
        for (std::size_t burst = 0; burst < preambles.size(); ++burst) {
            const std::size_t payloadOffset = preambles[burst] + 8;
            const std::size_t payloadBytes = burst + 1 < preambles.size()
                                                 ? preambles[burst + 1] - payloadOffset
                                                 : dataBytes - payloadOffset;
            const auto logical = UnswapMatTransportWords(
                data + payloadOffset, payloadBytes);
            const auto markers = FindBytePattern(logical, fullBandMarker);
            const auto lfe = FindBytePattern(logical, lfeMarker);
            if (markers.size() != slotsPerHalf * 2 || lfe.size() != 2) continue;
            bool valid = true;
            for (const std::size_t marker : markers) {
                valid = valid && marker + fullBandMarker.size() + 960 <= logical.size();
            }
            for (const std::size_t marker : lfe) {
                valid = valid && marker + lfeMarker.size() + 240 <= logical.size();
            }
            if (!valid) continue;
            for (std::size_t half = 0; half < 2; ++half) {
                for (std::size_t slot = 0; slot < slotsPerHalf; ++slot) {
                    const BYTE* source = logical.data() +
                        markers[half * slotsPerHalf + slot] + fullBandMarker.size();
                    AccumulatePcmLevel(slotLevels[slot], source, 480, true);
                    AccumulatePcmLevel(halfSlotLevels[half][slot], source, 480, true);
                }
                const BYTE* source = logical.data() + lfe[half] + lfeMarker.size();
                AccumulatePcmLevel(lfeLevel, source, 120, true);
                AccumulatePcmLevel(halfLfeLevels[half], source, 120, true);
            }
            ++analyzedBursts;
        }
        std::wcout << L"PCM slot levels across " << analyzedBursts << L" bursts ("
                   << slotsPerHalf << L" full-band slots per half):\n";
        for (std::size_t slot = 0; slot < slotLevels.size(); ++slot) {
            const PcmLevelAccumulator& level = slotLevels[slot];
            const double rms = level.samples == 0 ? 0.0 :
                std::sqrt(static_cast<double>(level.squareSum / level.samples));
            const double mean = level.samples == 0 ? 0.0 :
                static_cast<double>(level.sum / level.samples);
            const auto halfRms = [&](const std::size_t half) {
                const PcmLevelAccumulator& halfLevel = halfSlotLevels[half][slot];
                return halfLevel.samples == 0 ? 0.0 : std::sqrt(static_cast<double>(
                    halfLevel.squareSum / halfLevel.samples));
            };
            std::wcout << L"  slot " << std::setw(2) << slot << L": RMS "
                       << std::fixed << std::setprecision(2) << std::setw(7)
                       << Decibels(rms) << L", peak " << std::setw(7)
                       << Decibels(level.peak) << L", DC " << std::setw(7)
                       << Decibels(std::abs(mean)) << L", active blocks "
                       << level.activeBlocks << L", halves "
                       << halfSlotLevels[0][slot].activeBlocks << L"/"
                       << halfSlotLevels[1][slot].activeBlocks << L" @ "
                       << Decibels(halfRms(0)) << L"/" << Decibels(halfRms(1))
                       << L" dBFS\n";
        }
        const double lfeRms = lfeLevel.samples == 0 ? 0.0 :
            std::sqrt(static_cast<double>(lfeLevel.squareSum / lfeLevel.samples));
        const double lfeMean = lfeLevel.samples == 0 ? 0.0 :
            static_cast<double>(lfeLevel.sum / lfeLevel.samples);
        const auto lfeHalfRms = [&](const std::size_t half) {
            const PcmLevelAccumulator& level = halfLfeLevels[half];
            return level.samples == 0 ? 0.0 : std::sqrt(static_cast<double>(
                level.squareSum / level.samples));
        };
        std::wcout << L"  LFE    : RMS " << std::setw(7) << Decibels(lfeRms)
                   << L", peak " << std::setw(7) << Decibels(lfeLevel.peak)
                   << L", DC " << std::setw(7) << Decibels(std::abs(lfeMean))
                   << L", active blocks " << lfeLevel.activeBlocks << L", halves "
                   << halfLfeLevels[0].activeBlocks << L"/"
                   << halfLfeLevels[1].activeBlocks << L" @ "
                   << Decibels(lfeHalfRms(0)) << L"/" << Decibels(lfeHalfRms(1))
                   << L" dBFS\n";
    }
    if (fullBandMarkers.size() >= 31) {
        std::wcout << L"Active first-half PCM slots (RMS above -60 dBFS):\n";
        bool foundActiveSlot = false;
        for (std::size_t slot = 0; slot < 31; ++slot) {
            const std::size_t sampleOffset = fullBandMarkers[slot] + fullBandMarker.size();
            if (sampleOffset + 960 > logicalPayload.size()) break;
            const PcmWindowMatch match = FindPcmToneWindow(
                logicalPayload.data() + sampleOffset, 960, 900.0, true);
            if (match.rms < 0.001) continue;
            foundActiveSlot = true;
            std::wcout << L"  slot " << slot << L": RMS " << std::setprecision(2)
                       << Decibels(match.rms) << L" dBFS, 900 Hz "
                       << Decibels(match.amplitude) << L" dBFS, purity="
                       << std::setprecision(3) << match.score << L"\n";
        }
        if (!foundActiveSlot) std::wcout << L"  none\n";
    }
    for (const double frequency : probeFrequencies) {
        PcmWindowMatch match = FindPcmToneWindow(data + stableOffset, stableBytes, frequency, false);
        const PcmWindowMatch bigEndianMatch =
            FindPcmToneWindow(data + stableOffset, stableBytes, frequency, true);
        const bool bigEndian = bigEndianMatch.score > match.score;
        if (bigEndian) match = bigEndianMatch;
        PcmWindowMatch secondMatch = FindPcmToneWindow(
            data + stableOffset + secondHalfOffset, secondHalfBytes, frequency, false);
        const PcmWindowMatch secondBigEndianMatch = FindPcmToneWindow(
            data + stableOffset + secondHalfOffset, secondHalfBytes, frequency, true);
        const bool secondBigEndian = secondBigEndianMatch.score > secondMatch.score;
        if (secondBigEndian) secondMatch = secondBigEndianMatch;
        if (match.score == 0.0) {
            std::wcout << L"  " << std::setw(6) << std::setprecision(1) << frequency
                       << L" Hz: none\n";
            continue;
        }
        std::wcout << L"  " << std::setw(6) << std::setprecision(1) << frequency
                   << L" Hz: payload+" << match.offset << L", " << std::setprecision(2)
                   << Decibels(match.amplitude) << L" dBFS, purity="
                   << std::setprecision(3) << match.score
                   << (bigEndian ? L", BE" : L", LE");
        if (secondMatch.score != 0.0) {
            std::wcout << L"; second payload+" << (secondHalfOffset + secondMatch.offset)
                       << L", " << std::setprecision(2) << Decibels(secondMatch.amplitude)
                       << L" dBFS, purity=" << std::setprecision(3) << secondMatch.score
                       << (secondBigEndian ? L", BE" : L", LE");
        }
        std::wcout << L"\n";
    }

    constexpr std::size_t lfeSearchStart = 2'900;
    constexpr std::size_t lfeSearchBytes = 600;
    PcmWindowMatch lfeMatch;
    bool lfeBigEndian = false;
    for (const bool bigEndian : {false, true}) {
        PcmWindowMatch candidate = FindPcmToneWindow(
            data + stableOffset + lfeSearchStart, lfeSearchBytes, 55.0,
            bigEndian, 120, 12'000.0);
        if (candidate.score > lfeMatch.score) {
            lfeMatch = candidate;
            lfeBigEndian = bigEndian;
        }
    }
    std::wcout << L"LFE 12 kHz candidate: payload+" << (lfeSearchStart + lfeMatch.offset)
               << L", " << std::setprecision(2) << Decibels(lfeMatch.amplitude)
               << L" dBFS, purity=" << std::setprecision(3) << lfeMatch.score
               << (lfeBigEndian ? L", BE" : L", LE") << L"\n";

}

struct MatPositionCode {
    std::array<BYTE, 3> raw{};
    unsigned x{};
    unsigned z{};
    unsigned y{};
    unsigned flags{};
};

bool SameMatPositionCode(const MatPositionCode& first, const MatPositionCode& second) {
    return first.raw == second.raw;
}

MatPositionCode DecodeMatPositionCode(const std::vector<BYTE>& payload,
                                      const std::size_t offset) {
    if (offset + 3 > payload.size()) {
        throw std::runtime_error("MAT payload is too short for the position code");
    }
    const std::array<BYTE, 3> raw = {
        payload[offset], payload[offset + 1], payload[offset + 2]};
    return {
        raw,
        static_cast<unsigned>(raw[0] >> 2),
        static_cast<unsigned>(((raw[0] & 0x03) << 4) | (raw[1] >> 4)),
        static_cast<unsigned>(((raw[1] & 0x0f) << 2) | (raw[2] >> 6)),
        static_cast<unsigned>(raw[2] & 0x3f),
    };
}

struct MatPositionFixture {
    MatPositionCode code;
    std::size_t copies{};
    std::size_t unstableCopies{};
    std::vector<BYTE> comparisonPayload;
    std::vector<std::array<unsigned, 6>> objectTimeline;
};

MatPositionFixture ReadMatPositionFixture(const std::filesystem::path& inputPath) {
    const WaveImage image = ReadWaveImage(inputPath);
    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(image.formatBytes.data());
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        image.formatBytes.size() < sizeof(WAVEFORMATEXTENSIBLE) ||
        format->nChannels != 8 || format->nSamplesPerSec != 192'000 ||
        format->wBitsPerSample != 16 || format->nBlockAlign != 16) {
        throw std::runtime_error("Position comparison requires an 8-channel MAT carrier");
    }
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    if (!IsEqualGUID(extensible->SubFormat, kDolbyMat21Profile3)) {
        throw std::runtime_error("Position comparison requires MAT 2.1 Profile 3");
    }

    const BYTE* data = image.bytes.data() + image.dataOffset;
    std::vector<std::size_t> preambles;
    for (std::size_t offset = 0; offset + 8 <= image.dataBytes; ++offset) {
        if (data[offset] == 0x72 && data[offset + 1] == 0xf8 &&
            data[offset + 2] == 0x1f && data[offset + 3] == 0x4e &&
            ReadLittleUint16(data + offset + 4) == 0x16) {
            preambles.push_back(offset);
        }
    }
    if (preambles.size() < 5) {
        throw std::runtime_error("Position fixture contains too few MAT bursts");
    }

    // A 20 ms MAT burst repeats the object table in both 10 ms halves. The
    // second table moves slightly with the preceding MAT block, so locate its
    // stable header rather than assuming an absolute offset.
    MatPositionFixture fixture;
    bool haveReference = false;
    for (std::size_t burst = 3; burst + 1 < preambles.size(); ++burst) {
        const std::size_t payloadOffset = preambles[burst] + 8;
        const std::size_t payloadBytes = preambles[burst + 1] - payloadOffset;
        const auto payload = UnswapMatTransportWords(data + payloadOffset, payloadBytes);
        const auto tableOffsets = FindBytePattern(payload, kMatPositionTablePrefix);
        if (tableOffsets.size() != 2) continue;
        for (const std::size_t tableOffset : tableOffsets) {
            const MatPositionCode code = DecodeMatPositionCode(
                payload, tableOffset + kMatPositionTablePrefix.size());
            if (!haveReference) {
                fixture.code = code;
                haveReference = true;
            }
            ++fixture.copies;
            if (!SameMatPositionCode(code, fixture.code)) ++fixture.unstableCopies;
            fixture.objectTimeline.push_back(DecodeMatObjectFields(payload, tableOffset, 0));
        }
        if (burst == 10 || fixture.comparisonPayload.empty()) {
            fixture.comparisonPayload = payload;
        }
    }
    if (!haveReference || fixture.comparisonPayload.empty()) {
        throw std::runtime_error("No stable MAT position metadata was found");
    }
    return fixture;
}

void AnalyzeMatPositionTimeline(const std::filesystem::path& inputPath) {
    const MatPositionFixture fixture = ReadMatPositionFixture(inputPath);
    std::array<unsigned, 6> minimum{};
    std::array<unsigned, 6> maximum{};
    std::array<std::array<bool, 64>, 6> seen{};
    minimum.fill(63);
    for (const auto& object : fixture.objectTimeline) {
        for (std::size_t field = 0; field < object.size(); ++field) {
            minimum[field] = std::min(minimum[field], object[field]);
            maximum[field] = std::max(maximum[field], object[field]);
            seen[field][object[field]] = true;
        }
    }

    std::wcout << L"MAT object 0 metadata timeline: " << inputPath.wstring() << L"\n"
               << L"Samples: " << fixture.objectTimeline.size() << L" (two per 20 ms burst)\n";
    for (std::size_t field = 0; field < minimum.size(); ++field) {
        const std::size_t distinct = static_cast<std::size_t>(
            std::count(seen[field].begin(), seen[field].end(), true));
        const wchar_t* label = field == 0 ? L"X" : field == 1 ? L"Z" :
                               field == 2 ? L"Y" : L"aux";
        std::wcout << L"  field " << field << L" (" << label << L"): "
                   << minimum[field] << L".." << maximum[field]
                   << L", distinct=" << distinct << L"\n";
    }
    std::wcout << L"First object codes (X,Z,Y | aux3,aux4,aux5):\n";
    const std::size_t shown = std::min<std::size_t>(24, fixture.objectTimeline.size());
    for (std::size_t index = 0; index < shown; ++index) {
        const auto& fields = fixture.objectTimeline[index];
        std::wcout << L"  " << std::setw(3) << index << L": "
                   << fields[0] << L"," << fields[1] << L"," << fields[2]
                   << L" | " << fields[3] << L"," << fields[4] << L"," << fields[5]
                   << L"\n";
    }
}

void CompareMatPositionFixtures(const std::array<std::filesystem::path, 6>& inputPaths) {
    constexpr std::array<const wchar_t*, 6> names = {
        L"origin", L"left", L"right", L"above", L"front", L"behind"};
    std::array<MatPositionFixture, 6> fixtures;
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        fixtures[index] = ReadMatPositionFixture(inputPaths[index]);
    }

    std::wcout << L"Experimental MAT dynamic-object position codes:\n";
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        const MatPositionFixture& fixture = fixtures[index];
        std::wcout << L"  " << std::left << std::setw(7) << names[index] << std::right
                   << L" raw=" << std::hex << std::uppercase << std::setfill(L'0')
                   << std::setw(2) << static_cast<unsigned>(fixture.code.raw[0]) << L" "
                   << std::setw(2) << static_cast<unsigned>(fixture.code.raw[1]) << L" "
                   << std::setw(2) << static_cast<unsigned>(fixture.code.raw[2])
                   << std::dec << std::setfill(L' ')
                   << L"  X=" << fixture.code.x << L" Z=" << fixture.code.z
                   << L" Y=" << fixture.code.y << L" flags=" << fixture.code.flags
                   << L"  stable=" << (fixture.copies - fixture.unstableCopies)
                   << L"/" << fixture.copies << L"\n";
    }

    const auto& origin = fixtures[0].comparisonPayload;
    std::wcout << L"Changed logical-payload ranges versus origin (burst 10):\n";
    for (std::size_t fixtureIndex = 1; fixtureIndex < fixtures.size(); ++fixtureIndex) {
        const auto& candidate = fixtures[fixtureIndex].comparisonPayload;
        const std::size_t bytes = std::min(origin.size(), candidate.size());
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        for (std::size_t offset = 0; offset < bytes;) {
            if (origin[offset] == candidate[offset]) {
                ++offset;
                continue;
            }
            const std::size_t start = offset++;
            while (offset < bytes && origin[offset] != candidate[offset]) ++offset;
            ranges.emplace_back(start, offset - 1);
        }
        std::wcout << L"  " << std::left << std::setw(7) << names[fixtureIndex]
                   << std::right << L":";
        for (const auto& [start, end] : ranges) {
            std::wcout << L" " << start;
            if (end != start) std::wcout << L"-" << end;
        }
        std::wcout << L"\n";
    }
}

void AnalyzeFloatWave(const std::filesystem::path& inputPath) {
    std::ifstream stream(inputPath, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not open WAV file");

    char riff[4]{};
    std::uint32_t riffSize = 0;
    char wave[4]{};
    stream.read(riff, 4);
    stream.read(reinterpret_cast<char*>(&riffSize), sizeof(riffSize));
    stream.read(wave, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        throw std::runtime_error("Input is not a RIFF/WAVE file");
    }

    std::vector<BYTE> formatBytes;
    std::streampos dataPosition{};
    std::uint32_t dataBytes = 0;

    while (stream && (formatBytes.empty() || dataBytes == 0)) {
        char chunkId[4]{};
        std::uint32_t chunkSize = 0;
        stream.read(chunkId, 4);
        stream.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize));
        if (!stream) break;

        const std::streampos payloadPosition = stream.tellg();
        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            formatBytes.resize(chunkSize);
            stream.read(reinterpret_cast<char*>(formatBytes.data()), chunkSize);
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataPosition = payloadPosition;
            dataBytes = chunkSize;
        }
        stream.seekg(payloadPosition + static_cast<std::streamoff>(chunkSize + (chunkSize & 1U)));
    }

    if (formatBytes.size() < sizeof(WAVEFORMATEX) || dataBytes == 0) {
        throw std::runtime_error("WAV has no usable fmt/data chunks");
    }

    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(formatBytes.data());
    if (format->nChannels == 0 || format->wBitsPerSample != 32) {
        throw std::runtime_error("Analyzer currently requires 32-bit samples");
    }

    DWORD channelMask = 0;
    GUID subformat = kIeeeFloat;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        formatBytes.size() >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(formatBytes.data());
        channelMask = extensible->dwChannelMask;
        subformat = extensible->SubFormat;
    }
    if (!IsEqualGUID(subformat, kIeeeFloat)) {
        throw std::runtime_error("Analyzer currently requires IEEE float audio");
    }

    const std::size_t channels = format->nChannels;
    const auto names = ChannelNames(channelMask, format->nChannels);
    std::vector<long double> sums(channels, 0.0L);
    std::vector<long double> squares(channels, 0.0L);
    std::vector<long double> products(channels * channels, 0.0L);
    std::vector<double> peaks(channels, 0.0);
    std::vector<std::uint64_t> nonzero(channels, 0);
    std::uint64_t frames = 0;

    constexpr std::size_t framesPerRead = 8192;
    std::vector<float> samples(framesPerRead * channels);
    stream.clear();
    stream.seekg(dataPosition);
    std::uint32_t remaining = dataBytes;

    while (remaining != 0) {
        const auto requested = static_cast<std::streamsize>(
            std::min<std::uint32_t>(remaining,
                                    static_cast<std::uint32_t>(samples.size() * sizeof(float))));
        stream.read(reinterpret_cast<char*>(samples.data()), requested);
        const auto bytesRead = static_cast<std::size_t>(stream.gcount());
        if (bytesRead == 0) break;
        remaining -= static_cast<std::uint32_t>(bytesRead);
        const std::size_t blockFrames = bytesRead / (channels * sizeof(float));

        for (std::size_t frame = 0; frame < blockFrames; ++frame) {
            const float* values = samples.data() + frame * channels;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const long double value = values[channel];
                sums[channel] += value;
                squares[channel] += value * value;
                peaks[channel] = std::max(peaks[channel], std::abs(static_cast<double>(value)));
                if (std::abs(value) > 1.0e-8L) ++nonzero[channel];
                for (std::size_t other = channel; other < channels; ++other) {
                    products[channel * channels + other] += value * values[other];
                }
            }
        }
        frames += blockFrames;
    }

    if (frames == 0) throw std::runtime_error("WAV contains no complete frames");

    std::vector<double> means(channels);
    std::vector<double> deviations(channels);
    std::wcout << L"File: " << inputPath.wstring() << L"\n"
               << L"Format: " << WaveFormatText(format) << L"\n"
               << L"Frames: " << frames << L"\n"
               << L"Channel  RMS dBFS  Peak dBFS  Nonzero\n";

    for (std::size_t channel = 0; channel < channels; ++channel) {
        means[channel] = static_cast<double>(sums[channel] / frames);
        const double meanSquare = static_cast<double>(squares[channel] / frames);
        deviations[channel] = std::sqrt(std::max(0.0, meanSquare - means[channel] * means[channel]));
        const double rms = std::sqrt(meanSquare);
        std::wcout << L"  " << std::setw(4) << names[channel] << L"  "
                   << std::fixed << std::setprecision(2) << std::setw(8) << Decibels(rms) << L"  "
                   << std::setw(9) << Decibels(peaks[channel]) << L"  "
                   << std::setw(7) << (100.0 * nonzero[channel] / frames) << L"%\n";
    }

    bool printedCorrelation = false;
    for (std::size_t first = 0; first < channels; ++first) {
        for (std::size_t second = first + 1; second < channels; ++second) {
            if (deviations[first] == 0.0 || deviations[second] == 0.0) continue;
            const double meanProduct =
                static_cast<double>(products[first * channels + second] / frames);
            const double correlation =
                (meanProduct - means[first] * means[second]) /
                (deviations[first] * deviations[second]);
            if (std::abs(correlation) >= 0.98) {
                if (!printedCorrelation) std::wcout << L"Near-duplicate channel pairs:\n";
                printedCorrelation = true;
                std::wcout << L"  " << names[first] << L" / " << names[second]
                           << L": r=" << std::fixed << std::setprecision(4) << correlation << L"\n";
            }
        }
    }
    if (!printedCorrelation) std::wcout << L"Near-duplicate channel pairs: none\n";
}

} // namespace dolby
