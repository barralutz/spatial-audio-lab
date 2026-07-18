#include "commands.h"

#include "bridge_meter.h"
#include "mat_capture_client.h"
#include "multi_endpoint_renderer.h"
#include "speaker_layout.h"

#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace dolby {
namespace {

constexpr DWORD kPcm714ChannelMask = 0x0002D63F;
constexpr std::array<std::wstring_view, 12> kPcm714Channels = {
    L"FL", L"FR", L"FC", L"LFE", L"BL", L"BR",
    L"SL", L"SR", L"TFL", L"TFR", L"TBL", L"TBR",
};

std::array<std::size_t, kPcm714Channels.size()> BuildChannelMap(
    const SpeakerLayout& layout) {
    std::array<std::size_t, kPcm714Channels.size()> result{};
    for (std::size_t source = 0; source < kPcm714Channels.size(); ++source) {
        const auto destination = layout.FindSpeaker(kPcm714Channels[source]);
        if (!destination.has_value()) {
            throw std::runtime_error(
                "PCM 7.1.4 requires FL, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL and TBR");
        }
        result[source] = *destination;
    }
    return result;
}

void ValidatePcm714Format(const MAT_CAPTURE_READ_HEADER& header) {
    if (header.Version < MAT_CAPTURE_PROTOCOL_VERSION) {
        throw std::runtime_error(
            "PCM 7.1.4 requires capture driver protocol v3; install the rebuilt SysVAD driver");
    }
    if (!IsEqualGUID(header.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        header.SampleRate != 48'000 || header.Channels != 12 ||
        header.ChannelMask != kPcm714ChannelMask ||
        (header.BitsPerSample != 16 && header.BitsPerSample != 32) ||
        header.BlockAlign != header.Channels * (header.BitsPerSample / 8)) {
        throw std::runtime_error(
            "Capture endpoint is not emitting PCM 7.1.4 at 48 kHz with the expected channel mask");
    }
}

std::vector<std::int16_t> ConvertPcm714(
    const BYTE* bytes,
    const std::size_t byteCount,
    const MAT_CAPTURE_READ_HEADER& format,
    const SpeakerLayout& layout,
    const std::array<std::size_t, kPcm714Channels.size()>& channelMap,
    std::vector<BYTE>& carry) {
    carry.insert(carry.end(), bytes, bytes + byteCount);
    const std::size_t frameBytes = format.BlockAlign;
    const std::size_t frames = carry.size() / frameBytes;
    if (frames == 0) return {};

    std::vector<std::int16_t> output(frames * layout.speakers.size());
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const BYTE* sourceFrame = carry.data() + frame * frameBytes;
        for (std::size_t source = 0; source < channelMap.size(); ++source) {
            std::int16_t sample = 0;
            if (format.BitsPerSample == 16) {
                std::memcpy(&sample, sourceFrame + source * sizeof(sample), sizeof(sample));
            } else {
                std::int32_t wideSample = 0;
                std::memcpy(&wideSample, sourceFrame + source * sizeof(wideSample),
                            sizeof(wideSample));
                sample = static_cast<std::int16_t>(wideSample >> 16);
            }
            output[frame * layout.speakers.size() + channelMap[source]] = sample;
        }
    }

    const std::size_t consumed = frames * frameBytes;
    carry.erase(carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(consumed));
    return output;
}

} // namespace

void PlayLivePcmLayout(const double seconds,
                       const std::filesystem::path& layoutPath,
                       const double gain,
                       const DWORD prebufferMilliseconds,
                       const RendererLatencyMode latencyMode) {
    if (gain < 0.0 || gain > 1.0) {
        throw std::runtime_error("Live PCM layout gain must be between 0 and 1");
    }

    const SpeakerLayout layout = LoadSpeakerLayout(layoutPath);
    const auto channelMap = BuildChannelMap(layout);
    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());
    BridgeMeterPublisher meters(layout, BridgeMeterMode::Pcm);
    MultiEndpointRenderer renderer(layout, gain, latencyMode);
    InterleavedPcmQueue queue(layout.speakers.size());

    constexpr std::size_t requestPayloadBytes = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + requestPayloadBytes);
    std::vector<BYTE> carry;
    carry.reserve(4096);
    const std::uint64_t prebufferFrames = std::max<std::uint64_t>(
        std::max<std::uint64_t>(
            960, static_cast<std::uint64_t>(prebufferMilliseconds) * 48),
        renderer.MaximumBufferFrames());
    std::uint64_t expectedSequence = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t ringBytes = 0;
    std::uint64_t pcmFrames = 0;
    std::uint64_t maximumQueuedFrames = 0;
    bool haveSequence = false;
    bool acceptingInput = true;
    bool haveFormat = false;

    std::wcout << L"Live native PCM 7.1.4 layout: " << layout.name << L"\n"
               << L"  speakers=" << layout.speakers.size()
               << L", outputs=" << layout.outputs.size() << L"\n";
    for (const EndpointRenderStats& output : renderer.Stats()) {
        std::wcout << L"  " << output.routeName << L": " << output.endpointName
                   << (output.isMaster ? L" [master]" : L"")
                   << L", buffer=" << std::fixed << std::setprecision(2)
                   << output.bufferMilliseconds << L" ms, period="
                   << output.selectedPeriodFrames << L" frames"
                   << (output.lowLatencyApi ? L" [IAudioClient3]" : L"") << L"\n";
    }
    std::wcout << L"  decoder=none, input=PCM 7.1.4/48 kHz, gain=" << std::fixed
               << std::setprecision(2) << gain << L", prebuffer="
               << prebufferMilliseconds << L" ms, latency="
               << RendererLatencyModeName(latencyMode) << L", effective="
               << std::setprecision(2) << prebufferFrames / 48.0 << L" ms\n" << std::flush;

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
                    ValidatePcm714Format(read.header);
                    if (read.header.FormatChanges != 0) {
                        throw std::runtime_error("Capture format changed during PCM playback");
                    }
                    haveFormat = true;
                    if (haveSequence && read.header.FirstByteSequence != expectedSequence) {
                        sequenceGaps += read.header.FirstByteSequence > expectedSequence
                            ? read.header.FirstByteSequence - expectedSequence
                            : expectedSequence - read.header.FirstByteSequence;
                        carry.clear();
                    }
                    haveSequence = true;
                    expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
                    ringBytes += read.header.PayloadBytes;
                    lastPayloadTime = now;
                    auto pcm = ConvertPcm714(read.payload, read.header.PayloadBytes,
                                             read.header, layout, channelMap, carry);
                    if (!pcm.empty()) {
                        pcmFrames += pcm.size() / layout.speakers.size();
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
                renderer.PrimeAndStart(queue);
            }

            if (renderer.IsStarted()) {
                const bool activeInput = acceptingInput &&
                    now - lastPayloadTime < std::chrono::milliseconds(40);
                renderer.Service(queue, activeInput, 2);
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

    if (!haveFormat || !renderer.IsStarted()) {
        throw std::runtime_error("No PCM 7.1.4 frames arrived before the live timeout");
    }
    renderer.Stop();
    const MAT_CAPTURE_STATS ringStats = QueryMatCaptureStats(device.Get());
    std::wcout << L"Live native PCM layout playback complete\n"
               << L"  ring bytes=" << ringBytes << L", driver dropped="
               << ringStats.DroppedBytes << L", sequence gaps=" << sequenceGaps << L"\n"
               << L"  PCM frames=" << pcmFrames << L", trailing bytes=" << carry.size()
               << L", max PCM queue=" << maximumQueuedFrames << L" frames\n";
    PrintEndpointRenderStats(renderer.Stats());
}

} // namespace dolby
