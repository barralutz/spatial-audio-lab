#include "multi_endpoint_renderer.h"

#include "audio_platform.h"

#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dolby {

RendererLatencyMode ParseRendererLatencyMode(const std::wstring_view value) {
    const std::wstring normalized = Lowercase(std::wstring(value));
    if (normalized == L"safe") return RendererLatencyMode::Safe;
    if (normalized == L"balanced") return RendererLatencyMode::Balanced;
    if (normalized == L"low") return RendererLatencyMode::Low;
    throw std::runtime_error("Latency mode must be safe, balanced, or low");
}

std::wstring_view RendererLatencyModeName(const RendererLatencyMode mode) {
    switch (mode) {
    case RendererLatencyMode::Safe: return L"safe";
    case RendererLatencyMode::Balanced: return L"balanced";
    case RendererLatencyMode::Low: return L"low";
    }
    throw std::runtime_error("Unknown renderer latency mode");
}

namespace {

constexpr double kSampleRate = 48'000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kClockUpdateSeconds = 0.5;
constexpr double kControllerRecoveryFrames = kSampleRate * 20.0;
constexpr double kMaximumRateAdjustment = 0.002;
constexpr unsigned kEndpointRecoveryAttempts = 50;
constexpr DWORD kEndpointRecoveryDelayMilliseconds = 100;

class MmcssRegistration {
public:
    MmcssRegistration() {
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex_);
        if (handle_ != nullptr) AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
    }

    ~MmcssRegistration() {
        if (handle_ != nullptr) AvRevertMmThreadCharacteristics(handle_);
    }

    MmcssRegistration(const MmcssRegistration&) = delete;
    MmcssRegistration& operator=(const MmcssRegistration&) = delete;

    bool IsEnabled() const { return handle_ != nullptr; }

private:
    HANDLE handle_{};
    DWORD taskIndex_{};
};

double DbToLinear(const double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

Endpoint SelectUniqueEndpoint(const std::wstring& filter) {
    const std::wstring needle = Lowercase(filter);
    std::vector<Endpoint> matches;
    for (const Endpoint& endpoint : EnumerateRenderEndpoints()) {
        if (Lowercase(endpoint.name).find(needle) != std::wstring::npos ||
            Lowercase(endpoint.id).find(needle) != std::wstring::npos) {
            matches.push_back(endpoint);
        }
    }
    if (matches.size() != 1) {
        throw std::runtime_error("Output endpoint filter must match exactly one active device");
    }
    return matches.front();
}

struct RenderStream {
    std::wstring routeName;
    std::wstring endpointName;
    std::vector<std::size_t> speakerIndices;
    std::vector<double> speakerGains;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock> clock;
    WinHandle event;
    UINT32 bufferFrames{};
    UINT32 defaultPeriodFrames{};
    UINT32 minimumPeriodFrames{};
    UINT32 selectedPeriodFrames{};
    WORD channels{};
    REFERENCE_TIME streamLatency{};
    UINT64 clockFrequency{};
    UINT64 startClockPosition{};
    UINT64 endClockPosition{};
    UINT64 initialClockPosition{};
    UINT64 initialClockQpc{};
    UINT64 previousClockPosition{};
    UINT64 previousClockQpc{};
    double sourcePosition{};
    double configuredDelayFrames{};
    double resampleStep{1.0};
    double filteredClockRate{1.0};
    double relativeRatePpm{};
    double phaseErrorFrames{};
    double maximumPhaseErrorFrames{};
    std::uint64_t starvationFrames{};
    bool clockRateValid{};
    bool haveClockObservation{};
    bool running{};
    bool lowLatencyApi{};
};

UINT32 BalancedPeriod(const UINT32 defaultPeriod,
                      const UINT32 fundamentalPeriod,
                      const UINT32 minimumPeriod,
                      const UINT32 maximumPeriod) {
    const UINT32 fundamental = std::max<UINT32>(1, fundamentalPeriod);
    const UINT32 target = std::max(minimumPeriod, defaultPeriod / 2);
    const std::uint64_t aligned =
        (static_cast<std::uint64_t>(target) + fundamental - 1) / fundamental * fundamental;
    return static_cast<UINT32>(std::clamp<std::uint64_t>(
        aligned, minimumPeriod, maximumPeriod));
}

void ActivateLegacyClient(const Endpoint& endpoint, ComPtr<IAudioClient>* client) {
    client->Reset();
    ThrowIfFailed(endpoint.device->Activate(
                      __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                      reinterpret_cast<void**>(client->ReleaseAndGetAddressOf())),
                  "Activate multi-endpoint IAudioClient");
}

RenderStream OpenRenderStream(const SpeakerLayout& layout,
                              const OutputRouteDefinition& route,
                              const RendererLatencyMode latencyMode) {
    const Endpoint endpoint = SelectUniqueEndpoint(route.endpointFilter);
    RenderStream stream;
    stream.routeName = route.name;
    stream.endpointName = endpoint.name;
    stream.speakerIndices = route.speakerIndices;
    stream.channels = static_cast<WORD>(route.speakerIndices.size());
    stream.configuredDelayFrames = route.delayMilliseconds * kSampleRate / 1000.0;
    stream.sourcePosition = -stream.configuredDelayFrames;
    stream.speakerGains.reserve(route.speakerIndices.size());
    for (const std::size_t speakerIndex : route.speakerIndices) {
        stream.speakerGains.push_back(DbToLinear(layout.speakers[speakerIndex].trimDb));
    }

    ActivateLegacyClient(endpoint, &stream.client);
    ComPtr<IAudioClient3> client3;
    if (SUCCEEDED(stream.client.As(&client3))) {
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.eCategory = AudioCategory_Media;
        client3->SetClientProperties(&properties);
    }
    WAVEFORMATEX* mixFormat = nullptr;
    ThrowIfFailed(stream.client->GetMixFormat(&mixFormat), "Get multi-endpoint mix format");
    const bool validFormat = mixFormat->nChannels == stream.channels &&
                             mixFormat->nSamplesPerSec == static_cast<DWORD>(kSampleRate) &&
                             mixFormat->wBitsPerSample == 32 &&
                             IsFloatObjectFormat(mixFormat);
    if (!validFormat) {
        std::wcerr << L"Incompatible endpoint " << endpoint.name << L": "
                   << WaveFormatText(mixFormat) << L"\n";
        CoTaskMemFree(mixFormat);
        throw std::runtime_error("Multi-endpoint route does not match the device mix format");
    }
    constexpr DWORD streamFlags =
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    HRESULT initialize = E_NOINTERFACE;
    if (client3) {
        UINT32 fundamentalPeriod = 0;
        UINT32 maximumPeriod = 0;
        const HRESULT periods = client3->GetSharedModeEnginePeriod(
            mixFormat, &stream.defaultPeriodFrames, &fundamentalPeriod,
            &stream.minimumPeriodFrames, &maximumPeriod);
        if (SUCCEEDED(periods)) {
            stream.selectedPeriodFrames = stream.defaultPeriodFrames;
            if (latencyMode == RendererLatencyMode::Balanced) {
                stream.selectedPeriodFrames = BalancedPeriod(
                    stream.defaultPeriodFrames, fundamentalPeriod,
                    stream.minimumPeriodFrames, maximumPeriod);
            } else if (latencyMode == RendererLatencyMode::Low) {
                stream.selectedPeriodFrames = stream.minimumPeriodFrames;
            }
            if (latencyMode != RendererLatencyMode::Safe) {
                initialize = client3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    stream.selectedPeriodFrames, mixFormat, nullptr);
                stream.lowLatencyApi = SUCCEEDED(initialize);
                if (initialize == AUDCLNT_E_ENGINE_PERIODICITY_LOCKED) {
                    WAVEFORMATEX* currentFormat = nullptr;
                    UINT32 currentPeriod = 0;
                    if (SUCCEEDED(client3->GetCurrentSharedModeEnginePeriod(
                            &currentFormat, &currentPeriod))) {
                        CoTaskMemFree(currentFormat);
                        initialize = client3->InitializeSharedAudioStream(
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            currentPeriod, mixFormat, nullptr);
                        if (SUCCEEDED(initialize)) {
                            stream.selectedPeriodFrames = currentPeriod;
                            stream.lowLatencyApi = true;
                        }
                    }
                }
            }
        }
    }
    if (latencyMode == RendererLatencyMode::Safe || FAILED(initialize)) {
        if (latencyMode != RendererLatencyMode::Safe) {
            client3.Reset();
            ActivateLegacyClient(endpoint, &stream.client);
        }
        initialize = stream.client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0, mixFormat, nullptr);
        stream.lowLatencyApi = false;
    }
    CoTaskMemFree(mixFormat);
    ThrowIfFailed(initialize, "Initialize multi-endpoint render stream");

    stream.event = WinHandle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!stream.event.IsValid()) throw std::runtime_error("Create render event failed");
    ThrowIfFailed(stream.client->SetEventHandle(stream.event.Get()), "Set render event");
    ThrowIfFailed(stream.client->GetBufferSize(&stream.bufferFrames), "Get render buffer size");
    ThrowIfFailed(stream.client->GetStreamLatency(&stream.streamLatency),
                  "Get render stream latency");
    ThrowIfFailed(stream.client->GetService(IID_PPV_ARGS(&stream.render)),
                  "Get multi-endpoint render client");
    ThrowIfFailed(stream.client->GetService(IID_PPV_ARGS(&stream.clock)),
                  "Get multi-endpoint audio clock");
    ThrowIfFailed(stream.clock->GetFrequency(&stream.clockFrequency),
                  "Get multi-endpoint audio clock frequency");
    if (stream.clockFrequency == 0) throw std::runtime_error("Audio clock frequency is zero");
    return stream;
}

double InterpolatedSample(const InterleavedPcmQueue& queue,
                          const double sourcePosition,
                          const std::size_t channel) {
    const std::uint64_t first = static_cast<std::uint64_t>(std::floor(sourcePosition));
    const std::uint64_t second = std::min(first + 1, queue.EndFrame() - 1);
    const double fraction = sourcePosition - std::floor(sourcePosition);
    const double from = queue.Sample(first, channel);
    const double to = queue.Sample(second, channel);
    return from + (to - from) * fraction;
}

void FillStream(RenderStream& stream,
                const InterleavedPcmQueue& queue,
                const double gain,
                const bool countStarvation) {
    UINT32 padding = 0;
    ThrowIfFailed(stream.client->GetCurrentPadding(&padding), "Get render padding");
    const UINT32 available = stream.bufferFrames - padding;
    if (available == 0) return;

    BYTE* bytes = nullptr;
    ThrowIfFailed(stream.render->GetBuffer(available, &bytes), "Get render buffer");
    auto* output = reinterpret_cast<float*>(bytes);
    for (UINT32 outputFrame = 0; outputFrame < available; ++outputFrame) {
        const bool delaying = stream.sourcePosition < 0.0;
        const bool haveSource = !delaying &&
                                stream.sourcePosition < static_cast<double>(queue.EndFrame());
        for (WORD channel = 0; channel < stream.channels; ++channel) {
            double sample = 0.0;
            if (haveSource) {
                sample = InterpolatedSample(
                    queue, stream.sourcePosition, stream.speakerIndices[channel]);
                sample *= gain * stream.speakerGains[channel];
            }
            output[static_cast<std::size_t>(outputFrame) * stream.channels + channel] =
                static_cast<float>(std::clamp(sample, -1.0, 1.0));
        }
        if (delaying || haveSource) {
            stream.sourcePosition += stream.resampleStep;
        } else if (countStarvation) {
            ++stream.starvationFrames;
        }
    }
    ThrowIfFailed(stream.render->ReleaseBuffer(available, 0), "Release render buffer");
}

double PlayedSourcePosition(RenderStream& stream) {
    UINT32 padding = 0;
    ThrowIfFailed(stream.client->GetCurrentPadding(&padding), "Get synchronization padding");
    return stream.sourcePosition - static_cast<double>(padding) * stream.resampleStep;
}

void ObserveClock(RenderStream& stream) {
    UINT64 position = 0;
    UINT64 qpc = 0;
    ThrowIfFailed(stream.clock->GetPosition(&position, &qpc), "Read audio clock");
    if (!stream.haveClockObservation) {
        stream.initialClockPosition = position;
        stream.initialClockQpc = qpc;
    } else if (qpc > stream.initialClockQpc && position >= stream.initialClockPosition) {
        const double wallSeconds =
            static_cast<double>(qpc - stream.initialClockQpc) / 10'000'000.0;
        const double clockSeconds =
            static_cast<double>(position - stream.initialClockPosition) /
            static_cast<double>(stream.clockFrequency);
        if (wallSeconds >= 2.0) {
            const double cumulativeRate = clockSeconds / wallSeconds;
            if (cumulativeRate > 0.95 && cumulativeRate < 1.05) {
                stream.filteredClockRate = cumulativeRate;
                stream.clockRateValid = true;
            }
        }
    }
    stream.previousClockPosition = position;
    stream.previousClockQpc = qpc;
    stream.haveClockObservation = true;
}

} // namespace

InterleavedPcmQueue::InterleavedPcmQueue(const std::size_t channels) : channels_(channels) {
    if (channels == 0 || channels > 32) throw std::invalid_argument("Invalid PCM channel count");
}

void InterleavedPcmQueue::Append(std::vector<std::int16_t>&& samples) {
    if (samples.size() % channels_ != 0) {
        throw std::runtime_error("PCM producer returned an incomplete interleaved frame");
    }
    samples_.insert(samples_.end(),
                    std::make_move_iterator(samples.begin()),
                    std::make_move_iterator(samples.end()));
}

std::uint64_t InterleavedPcmQueue::EndFrame() const {
    return baseFrame_ + samples_.size() / channels_;
}

std::uint64_t InterleavedPcmQueue::FramesAvailable(const double sourcePosition) const {
    const double clamped = std::max(0.0, sourcePosition);
    const std::uint64_t first = static_cast<std::uint64_t>(std::floor(clamped));
    return first < EndFrame() ? EndFrame() - first : 0;
}

double InterleavedPcmQueue::Sample(const std::uint64_t frame, const std::size_t channel) const {
    if (frame < baseFrame_ || frame >= EndFrame() || channel >= channels_) return 0.0;
    return static_cast<double>(samples_[(frame - baseFrame_) * channels_ + channel]) / 32768.0;
}

void InterleavedPcmQueue::DiscardBefore(const double sourcePosition) {
    if (sourcePosition <= static_cast<double>(baseFrame_)) return;
    std::uint64_t frame = static_cast<std::uint64_t>(std::floor(sourcePosition));
    if (frame > 1) --frame;
    frame = std::min(frame, EndFrame());
    const std::uint64_t discardFrames = frame > baseFrame_ ? frame - baseFrame_ : 0;
    if (discardFrames < 4'800 && discardFrames != samples_.size() / channels_) return;
    const std::size_t discardSamples = static_cast<std::size_t>(discardFrames * channels_);
    samples_.erase(samples_.begin(), samples_.begin() + discardSamples);
    baseFrame_ += discardFrames;
}

struct MultiEndpointRenderer::Impl {
    MmcssRegistration mmcss;
    SpeakerLayout layout;
    double gain{};
    RendererLatencyMode latencyMode{};
    std::vector<RenderStream> streams;
    std::vector<HANDLE> events;
    std::chrono::steady_clock::time_point nextClockUpdate{};
    bool started{};
    bool stopped{};

    Impl(const SpeakerLayout& sourceLayout,
         const double sourceGain,
         const RendererLatencyMode sourceLatencyMode)
        : layout(sourceLayout), gain(sourceGain), latencyMode(sourceLatencyMode) {
        if (gain < 0.0 || gain > 1.0) {
            throw std::runtime_error("Multi-endpoint gain must be between 0 and 1");
        }
        streams.reserve(layout.outputs.size());
        events.reserve(layout.outputs.size());
        for (const OutputRouteDefinition& output : layout.outputs) {
            streams.push_back(OpenRenderStream(layout, output, latencyMode));
            events.push_back(streams.back().event.Get());
        }
    }

    void LogRouteFailure(const RenderStream& stream,
                         const wchar_t* operation,
                         const HRESULT result) const {
        std::wcerr << L"Renderer route '" << stream.routeName << L"' ("
                   << stream.endpointName << L"): " << operation << L" failed ("
                   << HResultText(result) << L")\n";
    }

    void StopStreams(const bool drain) {
        if (started && drain) {
            UINT32 maximumBuffer = 0;
            for (const RenderStream& stream : streams) {
                maximumBuffer = std::max(maximumBuffer, stream.bufferFrames);
            }
            Sleep(static_cast<DWORD>(
                std::ceil(maximumBuffer * 1000.0 / kSampleRate)) + 20);
        }
        for (RenderStream& stream : streams) {
            if (started) {
                UINT64 position = 0;
                const HRESULT clockResult = stream.clock->GetPosition(&position, nullptr);
                if (SUCCEEDED(clockResult)) {
                    stream.endClockPosition = position;
                } else {
                    LogRouteFailure(stream, L"read end clock", clockResult);
                }
            }
            if (stream.running) {
                const HRESULT stopResult = stream.client->Stop();
                if (FAILED(stopResult)) LogRouteFailure(stream, L"stop stream", stopResult);
            }
            stream.running = false;
        }
        started = false;
    }

    std::vector<double> SourcePositions() const {
        std::vector<double> positions;
        positions.reserve(streams.size());
        for (const RenderStream& stream : streams) positions.push_back(stream.sourcePosition);
        return positions;
    }

    void ReopenStreams(const std::vector<double>& sourcePositions) {
        StopStreams(false);

        std::vector<RenderStream> replacements;
        replacements.reserve(layout.outputs.size());
        for (std::size_t index = 0; index < layout.outputs.size(); ++index) {
            RenderStream stream = OpenRenderStream(layout, layout.outputs[index], latencyMode);
            if (index < sourcePositions.size()) stream.sourcePosition = sourcePositions[index];
            replacements.push_back(std::move(stream));
        }

        std::vector<HANDLE> replacementEvents;
        replacementEvents.reserve(replacements.size());
        for (const RenderStream& stream : replacements) {
            replacementEvents.push_back(stream.event.Get());
        }
        streams.swap(replacements);
        events.swap(replacementEvents);
        stopped = false;
    }

    void UpdateSynchronization(const bool sourceIsActive) {
        const auto now = std::chrono::steady_clock::now();
        if (now < nextClockUpdate) return;
        nextClockUpdate = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(kClockUpdateSeconds));
        for (RenderStream& stream : streams) ObserveClock(stream);

        RenderStream& master = streams[layout.masterOutput];
        master.resampleStep = 1.0;
        master.phaseErrorFrames = 0.0;
        if (!sourceIsActive) return;
        const double masterPlayed = PlayedSourcePosition(master);

        for (std::size_t index = 0; index < streams.size(); ++index) {
            if (index == layout.masterOutput) continue;
            RenderStream& stream = streams[index];
            const double played = PlayedSourcePosition(stream);
            stream.phaseErrorFrames =
                played + stream.configuredDelayFrames - masterPlayed;
            stream.maximumPhaseErrorFrames = std::max(
                stream.maximumPhaseErrorFrames, std::abs(stream.phaseErrorFrames));
            const double boundedPhaseError = std::clamp(
                stream.phaseErrorFrames, -kSampleRate * 0.05, kSampleRate * 0.05);
            const double targetStep = std::clamp(
                1.0 - boundedPhaseError / kControllerRecoveryFrames,
                1.0 - kMaximumRateAdjustment,
                1.0 + kMaximumRateAdjustment);
            stream.resampleStep = stream.resampleStep * 0.80 + targetStep * 0.20;
            if (stream.clockRateValid && master.clockRateValid) {
                stream.relativeRatePpm =
                    (stream.filteredClockRate / master.filteredClockRate - 1.0) * 1'000'000.0;
            }
        }
    }
};

MultiEndpointRenderer::MultiEndpointRenderer(const SpeakerLayout& layout,
                                             const double gain,
                                             const RendererLatencyMode latencyMode)
    : impl_(std::make_unique<Impl>(layout, gain, latencyMode)) {}

MultiEndpointRenderer::~MultiEndpointRenderer() {
    try {
        Stop();
    } catch (...) {
    }
}

void MultiEndpointRenderer::Prime(const InterleavedPcmQueue& queue) {
    if (impl_->started) throw std::runtime_error("Cannot prime a running renderer");
    for (RenderStream& stream : impl_->streams) FillStream(stream, queue, impl_->gain, false);
}

void MultiEndpointRenderer::Start() {
    if (impl_->started) return;
    std::vector<std::size_t> order;
    order.reserve(impl_->streams.size());
    for (std::size_t index = 0; index < impl_->streams.size(); ++index) {
        if (index != impl_->layout.masterOutput) order.push_back(index);
    }
    order.push_back(impl_->layout.masterOutput);
    try {
        for (const std::size_t index : order) {
            ThrowIfFailed(impl_->streams[index].client->Start(), "Start render stream");
            impl_->streams[index].running = true;
        }
        for (RenderStream& stream : impl_->streams) {
            ThrowIfFailed(stream.clock->GetPosition(&stream.startClockPosition, nullptr),
                          "Read render start clock");
            ObserveClock(stream);
        }
        impl_->nextClockUpdate = std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(kClockUpdateSeconds));
        impl_->started = true;
    } catch (...) {
        for (RenderStream& stream : impl_->streams) {
            if (stream.running) stream.client->Stop();
            stream.running = false;
        }
        throw;
    }
}

void MultiEndpointRenderer::RecoverPhysicalOutputs(
    const InterleavedPcmQueue& queue,
    const HRESULT failure,
    const std::vector<double>& sourcePositions,
    const std::wstring_view stage) {
    std::wcerr << L"WASAPI invalidated a physical output during " << stage << L" ("
               << HResultText(failure) << L"); reopening the configured routes.\n";
    for (unsigned attempt = 1; attempt <= kEndpointRecoveryAttempts; ++attempt) {
        try {
            impl_->ReopenStreams(sourcePositions);
            Prime(queue);
            Start();
            std::wcerr << L"Physical output recovery completed on attempt "
                       << attempt << L".\n";
            return;
        } catch (const AudioClientError& recoveryError) {
            if (!IsRecoverableAudioClientError(recoveryError.Result()) ||
                attempt == kEndpointRecoveryAttempts) {
                throw;
            }
            std::wcerr << L"Physical output recovery attempt " << attempt
                       << L" failed (" << HResultText(recoveryError.Result())
                       << L"); retrying.\n";
        } catch (const std::exception& recoveryError) {
            if (attempt == kEndpointRecoveryAttempts) throw;
            std::wcerr << L"Physical output recovery attempt " << attempt
                       << L" failed (" << recoveryError.what() << L"); retrying.\n";
        }
        Sleep(kEndpointRecoveryDelayMilliseconds);
    }
}

void MultiEndpointRenderer::PrimeAndStart(const InterleavedPcmQueue& queue) {
    const std::vector<double> sourcePositions = impl_->SourcePositions();
    try {
        Prime(queue);
        Start();
    } catch (const AudioClientError& error) {
        if (!IsRecoverableAudioClientError(error.Result())) throw;
        RecoverPhysicalOutputs(queue, error.Result(), sourcePositions, L"renderer startup");
    }
}

void MultiEndpointRenderer::Service(const InterleavedPcmQueue& queue,
                                    const bool countStarvation,
                                    const DWORD timeoutMilliseconds) {
    if (!impl_->started) throw std::runtime_error("Renderer must be started before servicing");
    try {
        const DWORD count = static_cast<DWORD>(impl_->events.size());
        const DWORD wait = WaitForMultipleObjects(
            count, impl_->events.data(), FALSE, timeoutMilliseconds);
        if (wait >= WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + count) {
            FillStream(impl_->streams[wait - WAIT_OBJECT_0], queue, impl_->gain, countStarvation);
        } else if (wait != WAIT_TIMEOUT) {
            throw std::runtime_error("Multi-endpoint render event failed");
        }
        for (std::size_t poll = 0; poll < impl_->events.size(); ++poll) {
            const DWORD ready = WaitForMultipleObjects(count, impl_->events.data(), FALSE, 0);
            if (ready < WAIT_OBJECT_0 || ready >= WAIT_OBJECT_0 + count) break;
            FillStream(impl_->streams[ready - WAIT_OBJECT_0], queue, impl_->gain, countStarvation);
        }
        impl_->UpdateSynchronization(countStarvation);
    } catch (const AudioClientError& error) {
        if (!IsRecoverableAudioClientError(error.Result())) throw;
        const std::vector<double> sourcePositions = impl_->SourcePositions();
        RecoverPhysicalOutputs(queue, error.Result(), sourcePositions, L"rendering");
    }
}

void MultiEndpointRenderer::DiscardConsumed(InterleavedPcmQueue& queue) const {
    double minimum = std::numeric_limits<double>::infinity();
    for (const RenderStream& stream : impl_->streams) {
        minimum = std::min(minimum, stream.sourcePosition);
    }
    queue.DiscardBefore(minimum);
}

std::uint64_t MultiEndpointRenderer::MinimumFramesAvailable(
    const InterleavedPcmQueue& queue) const {
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
    for (const RenderStream& stream : impl_->streams) {
        minimum = std::min(minimum, queue.FramesAvailable(stream.sourcePosition));
    }
    return minimum;
}

std::uint32_t MultiEndpointRenderer::MaximumBufferFrames() const {
    return std::max_element(
        impl_->streams.begin(), impl_->streams.end(),
        [](const RenderStream& first, const RenderStream& second) {
            return first.bufferFrames < second.bufferFrames;
        })->bufferFrames;
}

bool MultiEndpointRenderer::IsDrained(const InterleavedPcmQueue& queue) const {
    return std::all_of(impl_->streams.begin(), impl_->streams.end(), [&](const RenderStream& stream) {
        return queue.FramesAvailable(stream.sourcePosition) == 0;
    });
}

bool MultiEndpointRenderer::IsStarted() const {
    return impl_->started;
}

void MultiEndpointRenderer::Stop() {
    if (!impl_ || impl_->stopped) return;
    impl_->StopStreams(true);
    impl_->stopped = true;
}

const SpeakerLayout& MultiEndpointRenderer::Layout() const {
    return impl_->layout;
}

std::vector<EndpointRenderStats> MultiEndpointRenderer::Stats() const {
    std::vector<EndpointRenderStats> result;
    result.reserve(impl_->streams.size());
    double masterSeconds = 0.0;
    const RenderStream& master = impl_->streams[impl_->layout.masterOutput];
    if (master.endClockPosition >= master.startClockPosition) {
        masterSeconds = static_cast<double>(master.endClockPosition - master.startClockPosition) /
                        static_cast<double>(master.clockFrequency);
    }
    for (std::size_t index = 0; index < impl_->streams.size(); ++index) {
        const RenderStream& stream = impl_->streams[index];
        const double seconds = stream.endClockPosition >= stream.startClockPosition
                                   ? static_cast<double>(stream.endClockPosition -
                                                         stream.startClockPosition) /
                                         static_cast<double>(stream.clockFrequency)
                                   : 0.0;
        EndpointRenderStats stats;
        stats.routeName = stream.routeName;
        stats.endpointName = stream.endpointName;
        stats.isMaster = index == impl_->layout.masterOutput;
        stats.starvationFrames = stream.starvationFrames;
        stats.clockSeconds = seconds;
        stats.clockDeltaMilliseconds = (seconds - masterSeconds) * 1000.0;
        stats.relativeRatePpm = stream.relativeRatePpm;
        stats.resampleAdjustmentPpm = (stream.resampleStep - 1.0) * 1'000'000.0;
        stats.phaseErrorMilliseconds = stream.phaseErrorFrames * 1000.0 / kSampleRate;
        stats.maximumPhaseErrorMilliseconds =
            stream.maximumPhaseErrorFrames * 1000.0 / kSampleRate;
        stats.bufferFrames = stream.bufferFrames;
        stats.bufferMilliseconds = stream.bufferFrames * 1000.0 / kSampleRate;
        stats.streamLatencyMilliseconds = stream.streamLatency / 10'000.0;
        stats.defaultPeriodFrames = stream.defaultPeriodFrames;
        stats.minimumPeriodFrames = stream.minimumPeriodFrames;
        stats.selectedPeriodFrames = stream.selectedPeriodFrames;
        stats.lowLatencyApi = stream.lowLatencyApi;
        stats.mmcssEnabled = impl_->mmcss.IsEnabled();
        result.push_back(std::move(stats));
    }
    return result;
}

void PrintEndpointRenderStats(const std::vector<EndpointRenderStats>& stats) {
    std::wcout << L"Endpoint synchronization:\n";
    for (const EndpointRenderStats& stream : stats) {
        std::wcout << L"  " << stream.routeName << L" -> " << stream.endpointName
                   << (stream.isMaster ? L" [master]" : L"") << L"\n"
                   << L"    clock=" << std::fixed << std::setprecision(6)
                   << stream.clockSeconds << L" s, delta=" << std::setprecision(3)
                   << stream.clockDeltaMilliseconds << L" ms, rate="
                   << stream.relativeRatePpm << L" ppm, resample="
                   << stream.resampleAdjustmentPpm << L" ppm\n"
                   << L"    phase=" << stream.phaseErrorMilliseconds
                   << L" ms, max phase=" << stream.maximumPhaseErrorMilliseconds
                   << L" ms, starvation=" << stream.starvationFrames << L" frames\n"
                   << L"    buffer=" << stream.bufferFrames << L" frames / "
                   << stream.bufferMilliseconds << L" ms, stream latency="
                   << stream.streamLatencyMilliseconds << L" ms, period="
                   << stream.selectedPeriodFrames << L" frames (default="
                   << stream.defaultPeriodFrames << L", min="
                   << stream.minimumPeriodFrames << L"), IAudioClient3="
                   << (stream.lowLatencyApi ? L"yes" : L"no") << L", MMCSS="
                   << (stream.mmcssEnabled ? L"Pro Audio" : L"unavailable") << L"\n";
    }
}

void TestSpeakerLayout(const double seconds,
                       const SpeakerLayout& layout,
                       const double gain,
                       const std::wstring_view speakerName,
                       const RendererLatencyMode latencyMode) {
    if (seconds <= 0.0 || seconds > 3'600.0) {
        throw std::runtime_error("Layout test duration must be between 0 and 3600 seconds");
    }
    InterleavedPcmQueue queue(layout.speakers.size());
    MultiEndpointRenderer renderer(layout, gain, latencyMode);
    const std::uint64_t totalFrames = static_cast<std::uint64_t>(
        std::ceil(seconds * kSampleRate));
    constexpr std::uint64_t targetBufferedFrames = 24'000;
    constexpr std::uint64_t generationFrames = 960;
    std::uint64_t generatedFrames = 0;
    std::optional<std::size_t> selectedSpeaker;
    if (!speakerName.empty()) {
        selectedSpeaker = layout.FindSpeaker(speakerName);
        if (!selectedSpeaker.has_value()) {
            throw std::runtime_error("Speaker test references an unknown speaker");
        }
    }

    std::wcout << L"Layout clock test: " << layout.name << L"\n"
               << L"  speakers=" << layout.speakers.size()
               << L", outputs=" << layout.outputs.size()
               << L", duration=" << seconds << L" s, gain=" << gain;
    if (selectedSpeaker.has_value()) {
        std::wcout << L", speaker=" << layout.speakers[*selectedSpeaker].name;
    }
    std::wcout << L"\n";
    while (generatedFrames < totalFrames || !renderer.IsDrained(queue)) {
        while (generatedFrames < totalFrames &&
               renderer.MinimumFramesAvailable(queue) < targetBufferedFrames) {
            const std::uint64_t frames = std::min(generationFrames,
                                                   totalFrames - generatedFrames);
            std::vector<std::int16_t> samples(
                static_cast<std::size_t>(frames * layout.speakers.size()));
            for (std::uint64_t frame = 0; frame < frames; ++frame) {
                for (std::size_t channel = 0; channel < layout.speakers.size(); ++channel) {
                    if (selectedSpeaker.has_value() && channel != *selectedSpeaker) continue;
                    const double frequency = 220.0 + channel * 37.0;
                    const double phase = 2.0 * kPi * frequency *
                                         static_cast<double>(generatedFrames + frame) /
                                         kSampleRate;
                    samples[frame * layout.speakers.size() + channel] =
                        static_cast<std::int16_t>(std::lround(std::sin(phase) * 3'276.0));
                }
            }
            generatedFrames += frames;
            queue.Append(std::move(samples));
        }
        if (!renderer.IsStarted()) {
            renderer.PrimeAndStart(queue);
        } else {
            renderer.Service(queue, generatedFrames < totalFrames, 20);
            renderer.DiscardConsumed(queue);
        }
        if (generatedFrames >= totalFrames && renderer.IsDrained(queue)) break;
    }
    renderer.Stop();
    PrintEndpointRenderStats(renderer.Stats());
}

} // namespace dolby
