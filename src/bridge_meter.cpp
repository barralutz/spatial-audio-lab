#include "bridge_meter.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cwchar>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace dolby {
namespace {

constexpr wchar_t kMappingName[] = L"Local\\DolbyDecoderBridgeMetersV1";
constexpr std::uint32_t kMagic = 0x544D4244;
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kMaximumChannels = 32;
constexpr std::size_t kNameCharacters = 16;
constexpr double kSampleRate = 48'000.0;
constexpr double kRmsTimeConstantSeconds = 0.120;
constexpr double kPeakTimeConstantSeconds = 0.500;

struct BridgeMeterSharedData {
    std::uint32_t magic;
    std::uint32_t version;
    volatile LONG sequence;
    std::uint32_t channelCount;
    std::uint32_t mode;
    std::uint32_t reserved;
    std::uint64_t updateCounter;
    std::int64_t performanceCounter;
    float rms[kMaximumChannels];
    float peak[kMaximumChannels];
    wchar_t names[kMaximumChannels][kNameCharacters];
};

static_assert(offsetof(BridgeMeterSharedData, rms) == 40);
static_assert(offsetof(BridgeMeterSharedData, peak) == 168);
static_assert(offsetof(BridgeMeterSharedData, names) == 296);
static_assert(sizeof(BridgeMeterSharedData) == 1320);

void BeginWrite(BridgeMeterSharedData* data) {
    LONG sequence = InterlockedIncrement(&data->sequence);
    if ((sequence & 1) == 0) InterlockedIncrement(&data->sequence);
    MemoryBarrier();
}

void EndWrite(BridgeMeterSharedData* data) {
    MemoryBarrier();
    InterlockedIncrement(&data->sequence);
}

} // namespace

struct BridgeMeterPublisher::Impl {
    HANDLE mapping{};
    BridgeMeterSharedData* data{};
    std::size_t channels{};
    std::vector<double> rmsSquared;
    std::vector<double> peaks;

    Impl(const SpeakerLayout& layout, const BridgeMeterMode mode)
        : channels(layout.speakers.size()), rmsSquared(channels), peaks(channels) {
        if (channels == 0 || channels > kMaximumChannels) {
            throw std::runtime_error("Bridge meter layout has an unsupported channel count");
        }

        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(BridgeMeterSharedData), kMappingName);
        if (mapping == nullptr) throw std::runtime_error("Create bridge meter mapping failed");
        data = static_cast<BridgeMeterSharedData*>(
            MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BridgeMeterSharedData)));
        if (data == nullptr) {
            CloseHandle(mapping);
            mapping = nullptr;
            throw std::runtime_error("Map bridge meter shared memory failed");
        }

        BeginWrite(data);
        data->magic = kMagic;
        data->version = kVersion;
        data->channelCount = static_cast<std::uint32_t>(channels);
        data->mode = static_cast<std::uint32_t>(mode);
        data->reserved = 0;
        data->updateCounter = 0;
        data->performanceCounter = 0;
        std::fill(std::begin(data->rms), std::end(data->rms), 0.0F);
        std::fill(std::begin(data->peak), std::end(data->peak), 0.0F);
        std::memset(data->names, 0, sizeof(data->names));
        for (std::size_t channel = 0; channel < channels; ++channel) {
            wcsncpy_s(data->names[channel], kNameCharacters,
                      layout.speakers[channel].name.c_str(), _TRUNCATE);
        }
        EndWrite(data);
    }

    ~Impl() {
        if (data != nullptr) UnmapViewOfFile(data);
        if (mapping != nullptr) CloseHandle(mapping);
    }

    void Update(const std::vector<std::int16_t>& samples) {
        if (samples.empty()) return;
        if (samples.size() % channels != 0) {
            throw std::runtime_error("Bridge meter received an incomplete PCM frame");
        }

        const std::size_t frames = samples.size() / channels;
        std::array<long double, kMaximumChannels> squareSums{};
        std::array<double, kMaximumChannels> blockPeaks{};
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const double sample = static_cast<double>(samples[frame * channels + channel]) /
                                      32768.0;
                squareSums[channel] += sample * sample;
                blockPeaks[channel] = std::max(blockPeaks[channel], std::abs(sample));
            }
        }

        const double rmsAlpha = std::exp(
            -static_cast<double>(frames) / (kSampleRate * kRmsTimeConstantSeconds));
        const double peakAlpha = std::exp(
            -static_cast<double>(frames) / (kSampleRate * kPeakTimeConstantSeconds));
        BeginWrite(data);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const double blockMeanSquare = static_cast<double>(squareSums[channel] / frames);
            rmsSquared[channel] = rmsSquared[channel] * rmsAlpha +
                                  blockMeanSquare * (1.0 - rmsAlpha);
            peaks[channel] = std::max(blockPeaks[channel], peaks[channel] * peakAlpha);
            data->rms[channel] = static_cast<float>(std::sqrt(rmsSquared[channel]));
            data->peak[channel] = static_cast<float>(peaks[channel]);
        }
        ++data->updateCounter;
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        data->performanceCounter = counter.QuadPart;
        EndWrite(data);
    }
};

BridgeMeterPublisher::BridgeMeterPublisher(const SpeakerLayout& layout,
                                           const BridgeMeterMode mode)
    : impl_(std::make_unique<Impl>(layout, mode)) {}

BridgeMeterPublisher::~BridgeMeterPublisher() = default;

void BridgeMeterPublisher::Update(const std::vector<std::int16_t>& samples) {
    impl_->Update(samples);
}

} // namespace dolby
