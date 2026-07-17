#include "commands.h"

#include "audio_platform.h"
#include "bridge_meter.h"
#include "mat_capture_client.h"
#include "mat_format.h"
#include "multi_endpoint_renderer.h"
#include "speaker_layout.h"
#include "wave_io.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dolby {

struct MatStaticChannel {
    std::size_t fullBandSlot;
    std::size_t outputChannel;
    std::wstring_view speakerName;
    double azimuthDegrees;
    double height;
};

constexpr std::size_t kMatOutputFramesPerHalf = 480;
constexpr std::size_t kMatOutputChannels = 10;
constexpr std::size_t kMatFullBandSlotsPerHalf = 31;
constexpr std::array<BYTE, 6> kMatFullBandMarker = {0x83, 0x41, 0x43, 0xc2, 0x08, 0x2f};
constexpr std::array<BYTE, 6> kMatLfeMarker = {0x83, 0x41, 0x40, 0xf2, 0x0b, 0x2f};
constexpr std::size_t kMatLegacyStaticChannelCount = 9;
constexpr std::array<MatStaticChannel, 11> kMatStaticChannels = {{
    {0, 0, L"FL", -30.0, 0.0},
    {1, 1, L"FR", 30.0, 0.0},
    {2, 2, L"FC", 0.0, 0.0},
    {3, 6, L"SL", -90.0, 0.0},
    {4, 7, L"SR", 90.0, 0.0},
    {5, 4, L"BL", -150.0, 0.0},
    {6, 5, L"BR", 150.0, 0.0},
    {7, 8, L"TFL", -45.0, 1.0},
    {8, 9, L"TFR", 45.0, 1.0},
    {9, 8, L"TBL", -135.0, 1.0},
    {10, 9, L"TBR", 135.0, 1.0},
}};

std::int16_t ReadMatPcm16(const BYTE* sample, const bool bigEndian) {
    const std::uint16_t bits = bigEndian
                                   ? static_cast<std::uint16_t>((sample[0] << 8) | sample[1])
                                   : ReadLittleUint16(sample);
    return static_cast<std::int16_t>(bits);
}

std::array<double, 10> MatObject712Gains(const std::array<unsigned, 6>& fields) {
    constexpr double pi = 3.14159265358979323846;
    constexpr double degrees = pi / 180.0;
    struct RingSpeaker {
        double angle;
        std::size_t channel;
    };
    // Output order: FL, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR.
    constexpr std::array<RingSpeaker, 8> ring = {{
        {-150.0 * degrees, 4}, {-90.0 * degrees, 6}, {-30.0 * degrees, 0},
        {0.0, 2}, {30.0 * degrees, 1}, {90.0 * degrees, 7},
        {150.0 * degrees, 5}, {210.0 * degrees, 4},
    }};

    const double x = std::clamp((static_cast<double>(fields[0]) - 31.0) / 31.0,
                                -1.0, 1.0);
    const double z = std::clamp((static_cast<double>(fields[1]) - 31.0) / 31.0,
                                -1.0, 1.0);
    const double height = std::clamp((static_cast<double>(fields[2]) - 32.0) / 30.0,
                                     0.0, 1.0);
    double angle = std::abs(x) + std::abs(z) < 1.0e-6 ? 0.0 : std::atan2(x, -z);
    if (angle < ring.front().angle) angle += 2.0 * pi;

    std::array<double, 10> gains{};
    for (std::size_t index = 0; index + 1 < ring.size(); ++index) {
        if (angle < ring[index].angle || angle > ring[index + 1].angle) continue;
        const double fraction = (angle - ring[index].angle) /
                                (ring[index + 1].angle - ring[index].angle);
        gains[ring[index].channel] = std::cos(fraction * pi / 2.0);
        gains[ring[index + 1].channel] = std::sin(fraction * pi / 2.0);
        break;
    }

    const double bedScale = std::cos(height * pi / 2.0);
    for (std::size_t channel = 0; channel < 8; ++channel) gains[channel] *= bedScale;
    const double topScale = std::sin(height * pi / 2.0);
    const double topFraction = std::clamp((x + 1.0) / 2.0, 0.0, 1.0);
    gains[8] = topScale * std::cos(topFraction * pi / 2.0);
    gains[9] = topScale * std::sin(topFraction * pi / 2.0);
    return gains;
}

struct Mat712DecodeStats {
    std::size_t bursts{};
    std::size_t malformedBursts{};
    std::size_t objectMetadataFailures{};
    std::size_t activeDynamicObjectBlocks{};
    std::uint64_t clippedSamples{};
};

class Mat712Decoder {
public:
    Mat712Decoder() = default;

    explicit Mat712Decoder(const SpeakerLayout& layout)
        : layout_(&layout), outputChannels_(layout.speakers.size()) {
        staticGains_.reserve(kMatStaticChannels.size());
        for (const MatStaticChannel& channel : kMatStaticChannels) {
            std::vector<double> gains(outputChannels_);
            const auto exact = layout.FindSpeaker(channel.speakerName);
            if (exact.has_value()) {
                gains[*exact] = 1.0;
            } else {
                gains = PanDirection(layout, channel.azimuthDegrees, channel.height);
            }
            staticGains_.push_back(std::move(gains));
        }
        const auto lfe = layout.FindSpeaker(L"LFE");
        if (!lfe.has_value()) throw std::runtime_error("MAT layout does not contain LFE");
        lfeOutputChannel_ = *lfe;
    }

    std::vector<std::int16_t> DecodeBurst(const BYTE* payload, const std::size_t payloadBytes) {
        const auto logicalPayload = UnswapMatTransportWords(payload, payloadBytes);
        const auto fullBandMarkers = FindBytePattern(logicalPayload, kMatFullBandMarker);
        const auto lfeMarkers = FindBytePattern(logicalPayload, kMatLfeMarker);
        const auto objectTables = FindBytePattern(logicalPayload, kMatPositionTablePrefix);
        const auto validFullBandMarker = [&logicalPayload](const std::size_t offset) {
            return offset + kMatFullBandMarker.size() +
                       kMatOutputFramesPerHalf * sizeof(std::int16_t) <=
                   logicalPayload.size();
        };
        const auto validLfeMarker = [&logicalPayload](const std::size_t offset) {
            return offset + kMatLfeMarker.size() +
                       (kMatOutputFramesPerHalf / 4) * sizeof(std::int16_t) <=
                   logicalPayload.size();
        };
        const bool validMarkers = warmupBursts_ >= 3 &&
            fullBandMarkers.size() == kMatFullBandSlotsPerHalf * 2 &&
            lfeMarkers.size() == 2 &&
            std::all_of(fullBandMarkers.begin(), fullBandMarkers.end(), validFullBandMarker) &&
            std::all_of(lfeMarkers.begin(), lfeMarkers.end(), validLfeMarker);
        if (warmupBursts_ >= 3 && !validMarkers) ++stats_.malformedBursts;
        const bool validObjectMetadata = validMarkers && objectTables.size() == 2 &&
            std::all_of(objectTables.begin(), objectTables.end(),
                        [&logicalPayload](const std::size_t offset) {
                            constexpr std::size_t objectTableBytes = 20 * 36 / 8;
                            return offset + kMatPositionTablePrefix.size() + objectTableBytes <=
                                   logicalPayload.size();
                        });
        if (validMarkers && !validObjectMetadata) ++stats_.objectMetadataFailures;

        std::vector<std::int16_t> output(
            kMatOutputFramesPerHalf * 2 * outputChannels_, std::int16_t{0});
        std::vector<double> mixed(kMatOutputFramesPerHalf * outputChannels_);
        for (std::size_t half = 0; half < 2; ++half) {
            std::fill(mixed.begin(), mixed.end(), 0.0);
            std::int16_t* halfOutput =
                output.data() + half * kMatOutputFramesPerHalf * outputChannels_;
            if (!validMarkers) continue;

            const std::size_t staticChannelCount = layout_ == nullptr
                                                       ? kMatLegacyStaticChannelCount
                                                       : kMatStaticChannels.size();
            for (std::size_t staticIndex = 0;
                 staticIndex < staticChannelCount; ++staticIndex) {
                const MatStaticChannel& channel = kMatStaticChannels[staticIndex];
                const std::size_t markerIndex =
                    half * kMatFullBandSlotsPerHalf + channel.fullBandSlot;
                const BYTE* source = logicalPayload.data() + fullBandMarkers[markerIndex] +
                                     kMatFullBandMarker.size();
                for (std::size_t frame = 0; frame < kMatOutputFramesPerHalf; ++frame) {
                    const double sample = static_cast<double>(ReadMatPcm16(
                        source + frame * sizeof(std::int16_t), true)) / 32768.0;
                    if (layout_ == nullptr) {
                        mixed[frame * outputChannels_ + channel.outputChannel] += sample;
                    } else {
                        for (std::size_t outputChannel = 0;
                             outputChannel < outputChannels_; ++outputChannel) {
                            mixed[frame * outputChannels_ + outputChannel] +=
                                sample * staticGains_[staticIndex][outputChannel];
                        }
                    }
                }
            }

            const BYTE* lfeSource = logicalPayload.data() + lfeMarkers[half] +
                                    kMatLfeMarker.size();
            for (std::size_t frame = 0; frame < kMatOutputFramesPerHalf; ++frame) {
                mixed[frame * outputChannels_ + lfeOutputChannel_] +=
                    static_cast<double>(ReadMatPcm16(
                        lfeSource + (frame / 4) * sizeof(std::int16_t), true)) / 32768.0;
            }

            if (validObjectMetadata) {
                for (std::size_t object = 0; object < 20; ++object) {
                    const std::size_t markerIndex =
                        half * kMatFullBandSlotsPerHalf + 11 + object;
                    const BYTE* source = logicalPayload.data() + fullBandMarkers[markerIndex] +
                                         kMatFullBandMarker.size();
                    const auto fields = DecodeMatObjectFields(
                        logicalPayload, objectTables[half], object);
                    std::vector<double> gains;
                    if (layout_ != nullptr) {
                        gains = PanMatObject(*layout_, fields);
                    } else {
                        const auto legacyGains = MatObject712Gains(fields);
                        gains.assign(legacyGains.begin(), legacyGains.end());
                    }
                    bool active = false;
                    for (std::size_t frame = 0; frame < kMatOutputFramesPerHalf; ++frame) {
                        const std::int16_t sample = ReadMatPcm16(
                            source + frame * sizeof(std::int16_t), true);
                        active = active || sample != 0;
                        const double normalized = static_cast<double>(sample) / 32768.0;
                        for (std::size_t channel = 0; channel < outputChannels_; ++channel) {
                            mixed[frame * outputChannels_ + channel] +=
                                normalized * gains[channel];
                        }
                    }
                    if (active) ++stats_.activeDynamicObjectBlocks;
                }
            }

            for (std::size_t index = 0; index < mixed.size(); ++index) {
                if (mixed[index] < -1.0 || mixed[index] > 32767.0 / 32768.0) {
                    ++stats_.clippedSamples;
                }
                const double limited = std::clamp(mixed[index], -1.0, 32767.0 / 32768.0);
                halfOutput[index] = static_cast<std::int16_t>(
                    std::lround(limited * 32768.0));
            }
        }
        ++stats_.bursts;
        ++warmupBursts_;
        return output;
    }

    const Mat712DecodeStats& Stats() const { return stats_; }
    void ResetSynchronization() { warmupBursts_ = 0; }
    std::size_t OutputChannels() const { return outputChannels_; }

private:
    const SpeakerLayout* layout_{};
    std::size_t outputChannels_{kMatOutputChannels};
    std::size_t lfeOutputChannel_{3};
    std::vector<std::vector<double>> staticGains_;
    Mat712DecodeStats stats_;
    std::size_t warmupBursts_{};
};

class Mat712StreamFramer {
public:
    std::vector<std::int16_t> Push(const BYTE* data, const std::size_t bytes,
                                   Mat712Decoder& decoder) {
        constexpr std::size_t carrierBurstBytes = 61'440;
        constexpr std::size_t preambleBytes = 8;
        buffer_.insert(buffer_.end(), data, data + bytes);
        std::vector<std::int16_t> decodedPcm;

        while (true) {
            const std::size_t preamble = FindPreamble(cursor_);
            if (preamble == std::string::npos) {
                constexpr std::size_t retainedBytes = preambleBytes - 1;
                const std::size_t available = buffer_.size() - cursor_;
                if (available > retainedBytes) {
                    const std::size_t discard = available - retainedBytes;
                    cursor_ += discard;
                    discardedBytes_ += discard;
                }
                break;
            }
            if (preamble > cursor_) {
                discardedBytes_ += preamble - cursor_;
                cursor_ = preamble;
            }
            if (buffer_.size() - cursor_ < carrierBurstBytes) break;

            auto burst = decoder.DecodeBurst(
                buffer_.data() + cursor_ + preambleBytes,
                carrierBurstBytes - preambleBytes);
            decodedPcm.insert(decodedPcm.end(), burst.begin(), burst.end());
            cursor_ += carrierBurstBytes;
        }

        Compact();
        return decodedPcm;
    }

    void Reset() {
        discardedBytes_ += buffer_.size() - cursor_;
        buffer_.clear();
        cursor_ = 0;
    }

    std::uint64_t DiscardedBytes() const { return discardedBytes_; }
    std::size_t BufferedBytes() const { return buffer_.size() - cursor_; }

private:
    std::size_t FindPreamble(const std::size_t start) const {
        for (std::size_t offset = start; offset + 8 <= buffer_.size(); ++offset) {
            if (buffer_[offset] == 0x72 && buffer_[offset + 1] == 0xf8 &&
                buffer_[offset + 2] == 0x1f && buffer_[offset + 3] == 0x4e &&
                ReadLittleUint16(buffer_.data() + offset + 4) == 0x16) {
                return offset;
            }
        }
        return std::string::npos;
    }

    void Compact() {
        if (cursor_ == buffer_.size()) {
            buffer_.clear();
            cursor_ = 0;
        } else if (cursor_ >= 256U * 1024U) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + cursor_);
            cursor_ = 0;
        }
    }

    std::vector<BYTE> buffer_;
    std::size_t cursor_{};
    std::uint64_t discardedBytes_{};
};

void ExtractMat712Wave(const std::filesystem::path& inputPath,
                       const std::filesystem::path& outputPath) {
    const WaveImage image = ReadWaveImage(inputPath);
    const auto* inputFormat = reinterpret_cast<const WAVEFORMATEX*>(image.formatBytes.data());
    if (inputFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        image.formatBytes.size() < sizeof(WAVEFORMATEXTENSIBLE) ||
        inputFormat->nChannels != 8 || inputFormat->nSamplesPerSec != 192'000 ||
        inputFormat->wBitsPerSample != 16 || inputFormat->nBlockAlign != 16) {
        throw std::runtime_error("Extractor requires an 8-channel, 192 kHz, 16-bit MAT WAV");
    }
    const auto* inputExtensible =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(inputFormat);
    if (!IsEqualGUID(inputExtensible->SubFormat, kDolbyMat21Profile3)) {
        throw std::runtime_error("7.1.2 extraction currently requires MAT 2.1 Profile 3");
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
    if (preambles.empty()) throw std::runtime_error("No MAT IEC 61937 bursts were found");

    WAVEFORMATEXTENSIBLE outputFormat{};
    outputFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    outputFormat.Format.nChannels = 10;
    outputFormat.Format.nSamplesPerSec = 48'000;
    outputFormat.Format.wBitsPerSample = 16;
    outputFormat.Format.nBlockAlign =
        outputFormat.Format.nChannels * outputFormat.Format.wBitsPerSample / 8;
    outputFormat.Format.nAvgBytesPerSec =
        outputFormat.Format.nSamplesPerSec * outputFormat.Format.nBlockAlign;
    outputFormat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    outputFormat.Samples.wValidBitsPerSample = 16;
    outputFormat.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND |
                                 SPEAKER_TOP_FRONT_LEFT | SPEAKER_TOP_FRONT_RIGHT;
    outputFormat.SubFormat = kPcm;

    WaveWriter writer(outputPath, &outputFormat.Format);
    Mat712Decoder decoder;
    std::array<long double, kMatOutputChannels> squareSums{};
    std::array<std::int16_t, kMatOutputChannels> peaks{};
    std::uint64_t outputFrames = 0;
    std::size_t burstsWritten = 0;
    for (std::size_t burst = 0; burst < preambles.size(); ++burst) {
        const std::size_t preamble = preambles[burst];
        const std::size_t payloadOffset = preamble + 8;
        const std::size_t payloadBytes = burst + 1 < preambles.size()
                                             ? preambles[burst + 1] - payloadOffset
                                             : image.dataBytes - payloadOffset;
        if (payloadBytes < 60'000) break;
        const auto output = decoder.DecodeBurst(data + payloadOffset, payloadBytes);
        writer.Write(reinterpret_cast<const BYTE*>(output.data()), 960, false);
        for (std::size_t frame = 0; frame < 960; ++frame) {
            for (std::size_t channel = 0; channel < kMatOutputChannels; ++channel) {
                    const std::int16_t sample = output[frame * kMatOutputChannels + channel];
                    const long double normalized = static_cast<long double>(sample) / 32768.0L;
                    squareSums[channel] += normalized * normalized;
                    const std::int16_t magnitude = static_cast<std::int16_t>(
                        std::min<int>(std::abs(static_cast<int>(sample)), 32767));
                    peaks[channel] = std::max(peaks[channel], magnitude);
            }
        }
        outputFrames += 960;
        ++burstsWritten;
    }
    const Mat712DecodeStats& decodeStats = decoder.Stats();
    writer.Finalize();
    std::wcout << L"MAT 7.1.2 extraction written: " << outputPath.wstring() << L"\n"
               << L"Bursts: " << burstsWritten << L", frames: "
               << (burstsWritten * 960) << L", format: "
               << WaveFormatText(&outputFormat.Format) << L"\n";
    if (decodeStats.malformedBursts != 0) {
        std::wcout << L"Malformed bursts replaced with silence: "
                   << decodeStats.malformedBursts << L"\n";
    }
    std::wcout << L"Active dynamic-object blocks mixed: "
               << decodeStats.activeDynamicObjectBlocks << L"\n";
    if (decodeStats.objectMetadataFailures != 0) {
        std::wcout << L"Bursts without a decodable object table: "
                   << decodeStats.objectMetadataFailures << L"\n";
    }
    std::wcout << L"Clipped output samples: " << decodeStats.clippedSamples << L"\n";
    std::wcout << L"Warm-up bursts replaced with silence: "
               << std::min<std::size_t>(3, burstsWritten) << L"\n";
    constexpr std::array<std::wstring_view, kMatOutputChannels> channelNames = {
        L"FL", L"FR", L"FC", L"LFE", L"BL", L"BR", L"SL", L"SR", L"TFL", L"TFR"};
    std::wcout << L"Channel RMS / peak dBFS:\n";
    for (std::size_t channel = 0; channel < kMatOutputChannels; ++channel) {
        const double rms = outputFrames == 0
                               ? 0.0
                               : std::sqrt(static_cast<double>(squareSums[channel] / outputFrames));
        const double peak = static_cast<double>(peaks[channel]) / 32768.0;
        std::wcout << L"  " << std::setw(3) << channelNames[channel] << L": "
                   << std::fixed << std::setprecision(2) << std::setw(7) << Decibels(rms)
                   << L" / " << std::setw(7) << Decibels(peak) << L"\n";
    }
}

void AnalyzeMatLayout(const std::filesystem::path& inputPath,
                      const std::filesystem::path& layoutPath) {
    const WaveImage image = ReadWaveImage(inputPath);
    const auto* inputFormat = reinterpret_cast<const WAVEFORMATEX*>(image.formatBytes.data());
    if (inputFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        image.formatBytes.size() < sizeof(WAVEFORMATEXTENSIBLE) ||
        inputFormat->nChannels != 8 || inputFormat->nSamplesPerSec != 192'000 ||
        inputFormat->wBitsPerSample != 16 || inputFormat->nBlockAlign != 16) {
        throw std::runtime_error("Layout analyzer requires an 8-channel MAT WAV");
    }
    const auto* inputExtensible =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(inputFormat);
    if (!IsEqualGUID(inputExtensible->SubFormat, kDolbyMat21Profile3)) {
        throw std::runtime_error("Layout analyzer requires MAT 2.1 Profile 3");
    }

    const SpeakerLayout layout = LoadSpeakerLayout(layoutPath);
    const BYTE* data = image.bytes.data() + image.dataOffset;
    std::vector<std::size_t> preambles;
    for (std::size_t offset = 0; offset + 8 <= image.dataBytes; ++offset) {
        if (data[offset] == 0x72 && data[offset + 1] == 0xf8 &&
            data[offset + 2] == 0x1f && data[offset + 3] == 0x4e &&
            ReadLittleUint16(data + offset + 4) == 0x16) {
            preambles.push_back(offset);
        }
    }
    if (preambles.empty()) throw std::runtime_error("No MAT IEC 61937 bursts were found");

    Mat712Decoder decoder(layout);
    std::vector<long double> squareSums(layout.speakers.size());
    std::vector<double> peaks(layout.speakers.size());
    std::uint64_t outputFrames = 0;
    for (std::size_t burst = 0; burst < preambles.size(); ++burst) {
        const std::size_t payloadOffset = preambles[burst] + 8;
        const std::size_t payloadBytes = burst + 1 < preambles.size()
                                             ? preambles[burst + 1] - payloadOffset
                                             : image.dataBytes - payloadOffset;
        if (payloadBytes < 60'000) break;
        const auto output = decoder.DecodeBurst(data + payloadOffset, payloadBytes);
        for (std::size_t frame = 0; frame < 960; ++frame) {
            for (std::size_t channel = 0; channel < layout.speakers.size(); ++channel) {
                const double sample = static_cast<double>(
                    output[frame * layout.speakers.size() + channel]) / 32768.0;
                squareSums[channel] += sample * sample;
                peaks[channel] = std::max(peaks[channel], std::abs(sample));
            }
        }
        outputFrames += 960;
    }

    const Mat712DecodeStats& stats = decoder.Stats();
    std::wcout << L"MAT layout analysis: " << layout.name << L"\n"
               << L"  bursts=" << stats.bursts << L", frames=" << outputFrames
               << L", malformed=" << stats.malformedBursts
               << L", clipped=" << stats.clippedSamples << L"\n"
               << L"Channel RMS / peak dBFS:\n";
    for (std::size_t channel = 0; channel < layout.speakers.size(); ++channel) {
        const double rms = outputFrames == 0
                               ? 0.0
                               : std::sqrt(static_cast<double>(squareSums[channel] / outputFrames));
        std::wcout << L"  " << std::setw(4) << layout.speakers[channel].name << L": "
                   << std::fixed << std::setprecision(2) << std::setw(7) << Decibels(rms)
                   << L" / " << std::setw(7) << Decibels(peaks[channel]) << L"\n";
    }
}

struct AnalogRenderStream {
    std::wstring endpointName;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock> clock;
    HANDLE eventHandle{};
    UINT32 bufferFrames{};
    WORD channels{};
    std::size_t sourceChannel{};
    std::uint64_t sourceCursor{};
    UINT64 startClockPosition{};
};

AnalogRenderStream OpenAnalogRenderStream(const std::wstring& endpointFilter,
                                          const WORD expectedChannels,
                                          const std::size_t sourceChannel) {
    const Endpoint endpoint = SelectEndpoint(endpointFilter);
    AnalogRenderStream stream;
    stream.endpointName = endpoint.name;
    stream.channels = expectedChannels;
    stream.sourceChannel = sourceChannel;
    ThrowIfFailed(endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                             &stream.client),
                  "Activate analog render IAudioClient");

    WAVEFORMATEX* mixFormat = nullptr;
    ThrowIfFailed(stream.client->GetMixFormat(&mixFormat), "Get analog mix format");
    const bool validFormat = mixFormat->nChannels == expectedChannels &&
                             mixFormat->nSamplesPerSec == 48'000 &&
                             IsFloatObjectFormat(mixFormat);
    if (!validFormat) {
        const std::string text = "Analog endpoint has an incompatible mix format";
        CoTaskMemFree(mixFormat);
        throw std::runtime_error(text);
    }
    const HRESULT initialize = stream.client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat);
    ThrowIfFailed(initialize, "Initialize shared analog render stream");

    stream.eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (stream.eventHandle == nullptr) throw std::runtime_error("Create analog render event failed");
    ThrowIfFailed(stream.client->SetEventHandle(stream.eventHandle),
                  "Set analog render event");
    ThrowIfFailed(stream.client->GetBufferSize(&stream.bufferFrames),
                  "Get analog render buffer size");
    ThrowIfFailed(stream.client->GetService(IID_PPV_ARGS(&stream.render)),
                  "Get analog render client");
    ThrowIfFailed(stream.client->GetService(IID_PPV_ARGS(&stream.clock)),
                  "Get analog audio clock");
    return stream;
}

void FillAnalogRenderStream(AnalogRenderStream& stream, const BYTE* input,
                            const std::uint64_t loopFrames,
                            const std::uint64_t playbackFrames,
                            const double gain) {
    UINT32 padding = 0;
    ThrowIfFailed(stream.client->GetCurrentPadding(&padding), "Get analog render padding");
    const UINT32 available = stream.bufferFrames - padding;
    if (available == 0) return;

    BYTE* buffer = nullptr;
    ThrowIfFailed(stream.render->GetBuffer(available, &buffer), "Get analog render buffer");
    auto* output = reinterpret_cast<float*>(buffer);
    constexpr std::size_t inputChannels = 10;
    for (UINT32 frame = 0; frame < available; ++frame) {
        const bool haveSource = stream.sourceCursor < playbackFrames;
        for (WORD channel = 0; channel < stream.channels; ++channel) {
            float sample = 0.0f;
            if (haveSource) {
                const std::uint64_t inputFrame = stream.sourceCursor % loopFrames;
                const std::size_t inputIndex =
                    (inputFrame * inputChannels + stream.sourceChannel + channel) *
                    sizeof(std::int16_t);
                sample = static_cast<float>(
                    static_cast<std::int16_t>(ReadLittleUint16(input + inputIndex)) /
                    32768.0 * gain);
            }
            output[static_cast<std::size_t>(frame) * stream.channels + channel] = sample;
        }
        if (haveSource) ++stream.sourceCursor;
    }
    ThrowIfFailed(stream.render->ReleaseBuffer(available, 0),
                  "Release analog render buffer");
}

class LivePcm712Queue {
public:
    void Append(std::vector<std::int16_t>&& samples) {
        if (samples.size() % kMatOutputChannels != 0) {
            throw std::runtime_error("Live MAT decoder produced an incomplete PCM frame");
        }
        samples_.insert(samples_.end(),
                        std::make_move_iterator(samples.begin()),
                        std::make_move_iterator(samples.end()));
    }

    std::uint64_t EndFrame() const {
        return baseFrame_ + samples_.size() / kMatOutputChannels;
    }

    std::uint64_t FramesAvailable(const std::uint64_t cursor) const {
        return cursor < EndFrame() ? EndFrame() - cursor : 0;
    }

    std::int16_t Sample(const std::uint64_t frame, const std::size_t channel) const {
        if (frame < baseFrame_ || frame >= EndFrame() || channel >= kMatOutputChannels) {
            return 0;
        }
        return samples_[(frame - baseFrame_) * kMatOutputChannels + channel];
    }

    void DiscardBefore(const std::uint64_t frame) {
        const std::uint64_t discardFrames =
            std::min(frame, EndFrame()) > baseFrame_
                ? std::min(frame, EndFrame()) - baseFrame_
                : 0;
        if (discardFrames < 4'800 && discardFrames != samples_.size() / kMatOutputChannels) {
            return;
        }
        const std::size_t discardSamples =
            static_cast<std::size_t>(discardFrames * kMatOutputChannels);
        samples_.erase(samples_.begin(), samples_.begin() + discardSamples);
        baseFrame_ += discardFrames;
    }

private:
    std::vector<std::int16_t> samples_;
    std::uint64_t baseFrame_{};
};

void FillAnalogLiveStream(AnalogRenderStream& stream, const LivePcm712Queue& queue,
                          const double gain, std::uint64_t& starvationFrames,
                          const bool countStarvation) {
    UINT32 padding = 0;
    ThrowIfFailed(stream.client->GetCurrentPadding(&padding), "Get live analog render padding");
    const UINT32 available = stream.bufferFrames - padding;
    if (available == 0) return;

    BYTE* buffer = nullptr;
    ThrowIfFailed(stream.render->GetBuffer(available, &buffer),
                  "Get live analog render buffer");
    auto* output = reinterpret_cast<float*>(buffer);
    for (UINT32 frame = 0; frame < available; ++frame) {
        const bool haveSource = stream.sourceCursor < queue.EndFrame();
        for (WORD channel = 0; channel < stream.channels; ++channel) {
            const std::int16_t input = haveSource
                ? queue.Sample(stream.sourceCursor, stream.sourceChannel + channel)
                : 0;
            output[static_cast<std::size_t>(frame) * stream.channels + channel] =
                static_cast<float>(static_cast<double>(input) / 32768.0 * gain);
        }
        if (haveSource) {
            ++stream.sourceCursor;
        } else if (countStarvation) {
            ++starvationFrames;
        }
    }
    ThrowIfFailed(stream.render->ReleaseBuffer(available, 0),
                  "Release live analog render buffer");
}

void PlayAnalog712(const std::filesystem::path& inputPath,
                   const std::wstring& rearFilter,
                   const std::wstring& heightFilter,
                   const double gain,
                   const std::uint64_t repeatCount) {
    if (gain < 0.0 || gain > 1.0) {
        throw std::runtime_error("Analog playback gain must be between 0 and 1");
    }
    const WaveImage image = ReadWaveImage(inputPath);
    const auto* format = reinterpret_cast<const WAVEFORMATEX*>(image.formatBytes.data());
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        image.formatBytes.size() < sizeof(WAVEFORMATEXTENSIBLE) ||
        format->nChannels != 10 || format->nSamplesPerSec != 48'000 ||
        format->wBitsPerSample != 16 || format->nBlockAlign != 20) {
        throw std::runtime_error("play-712 requires a 10-channel PCM16/48 kHz WAV");
    }
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    if (!IsEqualGUID(extensible->SubFormat, kPcm)) {
        throw std::runtime_error("play-712 input subtype is not PCM");
    }
    const std::uint64_t inputFrames = image.dataBytes / format->nBlockAlign;
    if (inputFrames == 0 || repeatCount == 0 ||
        repeatCount > (48'000ULL * 3'600ULL) / inputFrames) {
        throw std::runtime_error("Analog playback repeat count is invalid or exceeds one hour");
    }
    const std::uint64_t playbackFrames = inputFrames * repeatCount;
    const BYTE* input = image.bytes.data() + image.dataOffset;

    AnalogRenderStream rear = OpenAnalogRenderStream(rearFilter, 8, 0);
    AnalogRenderStream height = OpenAnalogRenderStream(heightFilter, 2, 8);
    std::wcout << L"Analog 7.1 endpoint: " << rear.endpointName << L"\n"
               << L"Analog height endpoint: " << height.endpointName << L"\n"
               << L"Input: " << inputFrames << L" frames x " << repeatCount
               << L", gain=" << std::fixed << std::setprecision(2) << gain << L"\n";

    FillAnalogRenderStream(rear, input, inputFrames, playbackFrames, gain);
    FillAnalogRenderStream(height, input, inputFrames, playbackFrames, gain);
    ThrowIfFailed(rear.client->Start(), "Start rear analog stream");
    ThrowIfFailed(height.client->Start(), "Start height analog stream");
    ThrowIfFailed(rear.clock->GetPosition(&rear.startClockPosition, nullptr),
                  "Read rear start clock");
    ThrowIfFailed(height.clock->GetPosition(&height.startClockPosition, nullptr),
                  "Read height start clock");

    const std::array<HANDLE, 2> events = {rear.eventHandle, height.eventHandle};
    while (rear.sourceCursor < playbackFrames || height.sourceCursor < playbackFrames) {
        const DWORD wait = WaitForMultipleObjects(
            static_cast<DWORD>(events.size()), events.data(), FALSE, 2'000);
        if (wait == WAIT_OBJECT_0) {
            FillAnalogRenderStream(rear, input, inputFrames, playbackFrames, gain);
        } else if (wait == WAIT_OBJECT_0 + 1) {
            FillAnalogRenderStream(height, input, inputFrames, playbackFrames, gain);
        } else {
            rear.client->Stop();
            height.client->Stop();
            CloseHandle(rear.eventHandle);
            CloseHandle(height.eventHandle);
            throw std::runtime_error("Analog render event timed out");
        }
    }

    const DWORD drainMilliseconds = static_cast<DWORD>(std::ceil(
        1000.0 * std::max(rear.bufferFrames, height.bufferFrames) / 48'000.0)) + 20;
    Sleep(drainMilliseconds);
    UINT64 rearEnd = 0;
    UINT64 heightEnd = 0;
    UINT64 rearFrequency = 0;
    UINT64 heightFrequency = 0;
    ThrowIfFailed(rear.clock->GetPosition(&rearEnd, nullptr), "Read rear end clock");
    ThrowIfFailed(height.clock->GetPosition(&heightEnd, nullptr), "Read height end clock");
    ThrowIfFailed(rear.clock->GetFrequency(&rearFrequency), "Read rear clock frequency");
    ThrowIfFailed(height.clock->GetFrequency(&heightFrequency), "Read height clock frequency");
    ThrowIfFailed(rear.client->Stop(), "Stop rear analog stream");
    ThrowIfFailed(height.client->Stop(), "Stop height analog stream");
    CloseHandle(rear.eventHandle);
    CloseHandle(height.eventHandle);

    const double rearSeconds = static_cast<double>(rearEnd - rear.startClockPosition) /
                               static_cast<double>(rearFrequency);
    const double heightSeconds = static_cast<double>(heightEnd - height.startClockPosition) /
                                 static_cast<double>(heightFrequency);
    std::wcout << L"Analog playback complete. Rear clock=" << std::setprecision(6)
               << rearSeconds << L" s, height clock=" << heightSeconds
               << L" s, delta=" << std::setprecision(3)
               << (heightSeconds - rearSeconds) * 1000.0 << L" ms\n";
}

void PlayLiveMat712(const double seconds,
                    const std::wstring& rearFilter,
                    const std::wstring& heightFilter,
                    const double gain,
                    const DWORD prebufferMilliseconds) {
    if (gain < 0.0 || gain > 1.0) {
        throw std::runtime_error("Live analog gain must be between 0 and 1");
    }

    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());
    AnalogRenderStream rear = OpenAnalogRenderStream(rearFilter, 8, 0);
    AnalogRenderStream height = OpenAnalogRenderStream(heightFilter, 2, 8);
    Mat712Decoder decoder;
    Mat712StreamFramer framer;
    LivePcm712Queue queue;
    constexpr std::size_t requestPayloadBytes = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + requestPayloadBytes);
    const std::uint64_t prebufferFrames =
        std::max<std::uint64_t>(960, static_cast<std::uint64_t>(prebufferMilliseconds) * 48);
    std::uint64_t expectedSequence = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t ringBytes = 0;
    std::uint64_t rearStarvationFrames = 0;
    std::uint64_t heightStarvationFrames = 0;
    std::uint64_t maximumQueuedFrames = 0;
    bool haveSequence = false;
    bool started = false;
    bool acceptingInput = true;

    std::wcout << L"Live MAT 7.1.2\n"
               << L"  rear: " << rear.endpointName << L"\n"
               << L"  height: " << height.endpointName << L"\n"
               << L"  gain=" << std::fixed << std::setprecision(2) << gain
               << L", prebuffer=" << prebufferMilliseconds << L" ms\n";

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::duration<double>(seconds);
    auto lastPayloadTime = startTime;
    const std::array<HANDLE, 2> events = {rear.eventHandle, height.eventHandle};

    try {
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (acceptingInput) {
                const MatCaptureReadView read = ReadMatCapture(device.Get(), request);
                if (read.header.PayloadBytes != 0) {
                    if (haveSequence && read.header.FirstByteSequence != expectedSequence) {
                        sequenceGaps += read.header.FirstByteSequence > expectedSequence
                            ? read.header.FirstByteSequence - expectedSequence
                            : expectedSequence - read.header.FirstByteSequence;
                        framer.Reset();
                        decoder.ResetSynchronization();
                    }
                    haveSequence = true;
                    expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
                    ringBytes += read.header.PayloadBytes;
                    lastPayloadTime = now;
                    auto pcm = framer.Push(read.payload, read.header.PayloadBytes, decoder);
                    if (!pcm.empty()) {
                        queue.Append(std::move(pcm));
                        maximumQueuedFrames = std::max(
                            maximumQueuedFrames,
                            std::min(queue.FramesAvailable(rear.sourceCursor),
                                     queue.FramesAvailable(height.sourceCursor)));
                    }
                }

                if (now >= deadline) {
                    acceptingInput = false;
                }
            }

            const std::uint64_t queuedForBoth =
                std::min(queue.FramesAvailable(rear.sourceCursor),
                         queue.FramesAvailable(height.sourceCursor));
            if (!started && (queuedForBoth >= prebufferFrames ||
                             (!acceptingInput && queuedForBoth != 0))) {
                FillAnalogLiveStream(rear, queue, gain, rearStarvationFrames, false);
                FillAnalogLiveStream(height, queue, gain, heightStarvationFrames, false);
                ThrowIfFailed(rear.client->Start(), "Start live rear analog stream");
                ThrowIfFailed(height.client->Start(), "Start live height analog stream");
                ThrowIfFailed(rear.clock->GetPosition(&rear.startClockPosition, nullptr),
                              "Read live rear start clock");
                ThrowIfFailed(height.clock->GetPosition(&height.startClockPosition, nullptr),
                              "Read live height start clock");
                started = true;
            }

            if (started) {
                const bool countStarvation = acceptingInput &&
                    now - lastPayloadTime < std::chrono::milliseconds(20);
                const DWORD wait = WaitForMultipleObjects(
                    static_cast<DWORD>(events.size()), events.data(), FALSE, 2);
                if (wait == WAIT_OBJECT_0) {
                    FillAnalogLiveStream(
                        rear, queue, gain, rearStarvationFrames, countStarvation);
                } else if (wait == WAIT_OBJECT_0 + 1) {
                    FillAnalogLiveStream(
                        height, queue, gain, heightStarvationFrames, countStarvation);
                } else if (wait != WAIT_TIMEOUT) {
                    throw std::runtime_error("Live analog render event failed");
                }

                for (std::size_t extra = 0; extra < events.size(); ++extra) {
                    const DWORD ready = WaitForMultipleObjects(
                        static_cast<DWORD>(events.size()), events.data(), FALSE, 0);
                    if (ready == WAIT_OBJECT_0) {
                        FillAnalogLiveStream(
                            rear, queue, gain, rearStarvationFrames, countStarvation);
                    } else if (ready == WAIT_OBJECT_0 + 1) {
                        FillAnalogLiveStream(
                            height, queue, gain, heightStarvationFrames, countStarvation);
                    } else {
                        break;
                    }
                }
                queue.DiscardBefore(std::min(rear.sourceCursor, height.sourceCursor));
            } else {
                Sleep(2);
            }

            if (!acceptingInput) {
                if (!started ||
                    (queue.FramesAvailable(rear.sourceCursor) == 0 &&
                     queue.FramesAvailable(height.sourceCursor) == 0)) {
                    break;
                }
            }
        }
    } catch (...) {
        if (started) {
            rear.client->Stop();
            height.client->Stop();
        }
        CloseHandle(rear.eventHandle);
        CloseHandle(height.eventHandle);
        throw;
    }

    if (!started) {
        CloseHandle(rear.eventHandle);
        CloseHandle(height.eventHandle);
        throw std::runtime_error("No complete MAT bursts arrived before the live timeout");
    }

    const DWORD drainMilliseconds = static_cast<DWORD>(std::ceil(
        1000.0 * std::max(rear.bufferFrames, height.bufferFrames) / 48'000.0)) + 20;
    Sleep(drainMilliseconds);
    UINT64 rearEnd = 0;
    UINT64 heightEnd = 0;
    UINT64 rearFrequency = 0;
    UINT64 heightFrequency = 0;
    ThrowIfFailed(rear.clock->GetPosition(&rearEnd, nullptr), "Read live rear end clock");
    ThrowIfFailed(height.clock->GetPosition(&heightEnd, nullptr), "Read live height end clock");
    ThrowIfFailed(rear.clock->GetFrequency(&rearFrequency), "Read live rear clock frequency");
    ThrowIfFailed(height.clock->GetFrequency(&heightFrequency), "Read live height clock frequency");
    ThrowIfFailed(rear.client->Stop(), "Stop live rear analog stream");
    ThrowIfFailed(height.client->Stop(), "Stop live height analog stream");
    CloseHandle(rear.eventHandle);
    CloseHandle(height.eventHandle);

    const MAT_CAPTURE_STATS ringStats = QueryMatCaptureStats(device.Get());
    const Mat712DecodeStats& decodeStats = decoder.Stats();
    const double rearSeconds = static_cast<double>(rearEnd - rear.startClockPosition) /
                               static_cast<double>(rearFrequency);
    const double heightSeconds = static_cast<double>(heightEnd - height.startClockPosition) /
                                 static_cast<double>(heightFrequency);
    std::wcout << L"Live MAT playback complete\n"
               << L"  ring bytes=" << ringBytes << L", driver dropped="
               << ringStats.DroppedBytes << L", sequence gaps=" << sequenceGaps << L"\n"
               << L"  bursts=" << decodeStats.bursts << L", malformed="
               << decodeStats.malformedBursts << L", clipped=" << decodeStats.clippedSamples
               << L", framer discarded=" << framer.DiscardedBytes()
               << L", buffered=" << framer.BufferedBytes() << L"\n"
               << L"  max PCM queue=" << maximumQueuedFrames
               << L" frames, active starvation rear="
               << rearStarvationFrames << L", height=" << heightStarvationFrames << L"\n"
               << L"  rear clock=" << std::setprecision(6) << rearSeconds
               << L" s, height clock=" << heightSeconds << L" s, delta="
               << std::setprecision(3) << (heightSeconds - rearSeconds) * 1000.0 << L" ms\n";
}

void PlayLiveMatLayout(const double seconds,
                       const std::filesystem::path& layoutPath,
                       const double gain,
                       const DWORD prebufferMilliseconds,
                       const RendererLatencyMode latencyMode) {
    if (gain < 0.0 || gain > 1.0) {
        throw std::runtime_error("Live layout gain must be between 0 and 1");
    }

    const SpeakerLayout layout = LoadSpeakerLayout(layoutPath);
    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());
    BridgeMeterPublisher meters(layout, BridgeMeterMode::Mat);
    MultiEndpointRenderer renderer(layout, gain, latencyMode);
    Mat712Decoder decoder(layout);
    Mat712StreamFramer framer;
    InterleavedPcmQueue queue(layout.speakers.size());
    constexpr std::size_t requestPayloadBytes = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + requestPayloadBytes);
    const std::uint64_t prebufferFrames = std::max<std::uint64_t>(
        std::max<std::uint64_t>(
            960, static_cast<std::uint64_t>(prebufferMilliseconds) * 48),
        renderer.MaximumBufferFrames());
    std::uint64_t expectedSequence = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t ringBytes = 0;
    std::uint64_t maximumQueuedFrames = 0;
    bool haveSequence = false;
    bool acceptingInput = true;

    std::wcout << L"Live MAT configurable layout: " << layout.name << L"\n"
               << L"  speakers=" << layout.speakers.size()
               << L", outputs=" << layout.outputs.size() << L"\n";
    const auto initialStats = renderer.Stats();
    for (const EndpointRenderStats& output : initialStats) {
        std::wcout << L"  " << output.routeName << L": " << output.endpointName
                   << (output.isMaster ? L" [master]" : L"")
                   << L", buffer=" << std::fixed << std::setprecision(2)
                   << output.bufferMilliseconds << L" ms, period="
                   << output.selectedPeriodFrames << L" frames"
                   << (output.lowLatencyApi ? L" [IAudioClient3]" : L"") << L"\n";
    }
    std::wcout << L"  gain=" << std::fixed << std::setprecision(2) << gain
               << L", prebuffer=" << prebufferMilliseconds << L" ms, latency="
               << RendererLatencyModeName(latencyMode) << L", effective="
               << std::setprecision(2) << prebufferFrames / 48.0 << L" ms\n";

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::duration<double>(seconds);
    const bool unlimited = seconds == 0.0;
    auto lastPayloadTime = startTime;
    try {
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (acceptingInput) {
                const MatCaptureReadView read = ReadMatCapture(device.Get(), request);
                if (read.header.PayloadBytes != 0) {
                    if (haveSequence && read.header.FirstByteSequence != expectedSequence) {
                        sequenceGaps += read.header.FirstByteSequence > expectedSequence
                            ? read.header.FirstByteSequence - expectedSequence
                            : expectedSequence - read.header.FirstByteSequence;
                        framer.Reset();
                        decoder.ResetSynchronization();
                    }
                    haveSequence = true;
                    expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
                    ringBytes += read.header.PayloadBytes;
                    lastPayloadTime = now;
                    auto pcm = framer.Push(read.payload, read.header.PayloadBytes, decoder);
                    if (!pcm.empty()) {
                        meters.Update(pcm);
                        queue.Append(std::move(pcm));
                        maximumQueuedFrames = std::max(
                            maximumQueuedFrames, renderer.MinimumFramesAvailable(queue));
                    }
                }
                if (!unlimited && now >= deadline) acceptingInput = false;
            }

            const std::uint64_t queuedForAll = renderer.MinimumFramesAvailable(queue);
            if (!renderer.IsStarted() &&
                (queuedForAll >= prebufferFrames || (!acceptingInput && queuedForAll != 0))) {
                renderer.Prime(queue);
                renderer.Start();
            }

            if (renderer.IsStarted()) {
                const bool activeCarrier = acceptingInput &&
                    now - lastPayloadTime < std::chrono::milliseconds(40);
                renderer.Service(queue, activeCarrier, 2);
                renderer.DiscardConsumed(queue);
            } else {
                Sleep(2);
            }

            if (!acceptingInput && (!renderer.IsStarted() || renderer.IsDrained(queue))) break;
        }
    } catch (...) {
        renderer.Stop();
        throw;
    }

    if (!renderer.IsStarted()) {
        throw std::runtime_error("No complete MAT bursts arrived before the live timeout");
    }
    renderer.Stop();
    const MAT_CAPTURE_STATS ringStats = QueryMatCaptureStats(device.Get());
    const Mat712DecodeStats& decodeStats = decoder.Stats();
    std::wcout << L"Live MAT layout playback complete\n"
               << L"  ring bytes=" << ringBytes << L", driver dropped="
               << ringStats.DroppedBytes << L", sequence gaps=" << sequenceGaps << L"\n"
               << L"  bursts=" << decodeStats.bursts << L", malformed="
               << decodeStats.malformedBursts << L", clipped=" << decodeStats.clippedSamples
               << L", framer discarded=" << framer.DiscardedBytes()
               << L", buffered=" << framer.BufferedBytes() << L"\n"
               << L"  max PCM queue=" << maximumQueuedFrames << L" frames\n";
    PrintEndpointRenderStats(renderer.Stats());
}


} // namespace dolby
