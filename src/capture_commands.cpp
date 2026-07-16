#include "commands.h"

#include "audio_platform.h"
#include "wave_io.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace dolby {

enum class ProbeAudioClientActivationType : int {
    Default = 0,
    ProcessLoopback = 1,
};

enum class ProbeProcessLoopbackMode : int {
    IncludeTargetProcessTree = 0,
    ExcludeTargetProcessTree = 1,
};

struct ProbeAudioClientProcessLoopbackParams {
    DWORD targetProcessId;
    ProbeProcessLoopbackMode processLoopbackMode;
};

struct ProbeAudioClientActivationParams {
    ProbeAudioClientActivationType activationType;
    union {
        ProbeAudioClientProcessLoopbackParams processLoopbackParams;
    };
};

constexpr wchar_t kVirtualProcessLoopbackDevice[] = L"VAD\\Process_Loopback";

class ProcessLoopbackActivationHandler final
    : public IActivateAudioInterfaceCompletionHandler,
      public IAgileObject {
public:
    ProcessLoopbackActivationHandler() : completed_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        if (completed_ == nullptr) throw std::runtime_error("Create activation event failed");
    }

    ~ProcessLoopbackActivationHandler() { CloseHandle(completed_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) ||
            IsEqualIID(iid, __uuidof(IActivateAudioInterfaceCompletionHandler))) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (IsEqualIID(iid, __uuidof(IAgileObject))) {
            *object = static_cast<IAgileObject*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --references_;
        if (references == 0) delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE
    ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        ComPtr<IUnknown> audioInterface;
        HRESULT activateResult = E_UNEXPECTED;
        result_ = operation->GetActivateResult(&activateResult, &audioInterface);
        if (SUCCEEDED(result_)) result_ = activateResult;
        if (SUCCEEDED(result_)) result_ = audioInterface.As(&client_);
        SetEvent(completed_);
        return S_OK;
    }

    ComPtr<IAudioClient> WaitForClient() {
        const DWORD waitResult = WaitForSingleObject(completed_, 10'000);
        if (waitResult != WAIT_OBJECT_0) {
            throw std::runtime_error("Process-loopback activation timed out");
        }
        ThrowIfFailed(result_, "Activate process loopback");
        return client_;
    }

private:
    std::atomic<ULONG> references_{1};
    HANDLE completed_{};
    HRESULT result_{E_PENDING};
    ComPtr<IAudioClient> client_;
};

ComPtr<IAudioClient> ActivateProcessLoopback(const DWORD processId) {
    ProbeAudioClientActivationParams activation{};
    activation.activationType = ProbeAudioClientActivationType::ProcessLoopback;
    activation.processLoopbackParams.targetProcessId = processId;
    activation.processLoopbackParams.processLoopbackMode =
        ProbeProcessLoopbackMode::IncludeTargetProcessTree;

    PROPVARIANT parameters;
    PropVariantInit(&parameters);
    parameters.vt = VT_BLOB;
    parameters.blob.cbSize = sizeof(activation);
    parameters.blob.pBlobData = reinterpret_cast<BYTE*>(&activation);

    ComPtr<ProcessLoopbackActivationHandler> handler;
    handler.Attach(new ProcessLoopbackActivationHandler());
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    ThrowIfFailed(ActivateAudioInterfaceAsync(kVirtualProcessLoopbackDevice,
                                              __uuidof(IAudioClient), &parameters,
                                              handler.Get(), &operation),
                  "Start process-loopback activation");
    return handler->WaitForClient();
}
void FillTransportPattern(BYTE* data, const UINT32 frames, const WORD blockAlign,
                          std::uint64_t& nextFrame, std::uint64_t& preamblesWritten) {
    constexpr std::array<BYTE, 8> marker = {0x72, 0xf8, 0x1f, 0x4e, 'D', 'P', 'R', 'B'};
    const auto bytes = static_cast<std::size_t>(frames) * blockAlign;
    std::memset(data, 0, bytes);

    for (UINT32 frame = 0; frame < frames; ++frame, ++nextFrame) {
        if ((nextFrame % 256) != 0 || blockAlign < 16) continue;
        BYTE* destination = data + static_cast<std::size_t>(frame) * blockAlign;
        std::copy(marker.begin(), marker.end(), destination);
        std::memcpy(destination + marker.size(), &nextFrame, sizeof(nextFrame));
        ++preamblesWritten;
    }
}

void RenderTransportTest(const double seconds, const std::wstring& filter,
                         const std::wstring& mode) {
    Endpoint endpoint = SelectEndpoint(filter);
    std::wcout << L"Rendering to endpoint: " << endpoint.name << L"\n";

    const auto pcm = MakePcmFormat(8, KSAUDIO_SPEAKER_7POINT1_SURROUND);
    const auto mat20 = MakeIec61937Format(kDolbyMat20, 48000);
    const auto mat21 = MakeIec61937Format(kDolbyMat21Profile3, 48000);
    const WAVEFORMATEX* format = nullptr;
    std::wstring formatName;
    if (mode == L"pcm") {
        format = &pcm.Format;
        formatName = L"PCM 7.1";
    } else if (mode == L"mat20") {
        format = &mat20.formatExt.Format;
        formatName = L"Dolby MAT 2.0 transport";
    } else if (mode == L"mat21") {
        format = &mat21.formatExt.Format;
        formatName = L"Dolby MAT 2.1 Profile 3 transport";
    } else {
        throw std::runtime_error("Render mode must be pcm, mat20, or mat21");
    }

    ComPtr<IAudioClient> client;
    ThrowIfFailed(endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client),
                  "Activate render IAudioClient");
    const HRESULT support = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, format, nullptr);
    if (support != S_OK) ThrowIfFailed(support, "Check exclusive render format");

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    ThrowIfFailed(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod), "Get render device period");

    constexpr REFERENCE_TIME requestedDuration = 1'000'000; // 100 ms
    ThrowIfFailed(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     requestedDuration, requestedDuration, format, nullptr),
                  "Initialize exclusive render");

    HANDLE renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (renderEvent == nullptr) throw std::runtime_error("Create render event failed");
    const HRESULT eventResult = client->SetEventHandle(renderEvent);
    if (FAILED(eventResult)) {
        CloseHandle(renderEvent);
        ThrowIfFailed(eventResult, "Set render event");
    }

    UINT32 bufferFrames = 0;
    ThrowIfFailed(client->GetBufferSize(&bufferFrames), "Get render buffer size");
    ComPtr<IAudioRenderClient> render;
    ThrowIfFailed(client->GetService(IID_PPV_ARGS(&render)), "Get render client");

    std::uint64_t nextFrame = 0;
    std::uint64_t framesSubmitted = 0;
    std::uint64_t preamblesWritten = 0;
    BYTE* data = nullptr;
    ThrowIfFailed(render->GetBuffer(bufferFrames, &data), "Get initial render buffer");
    FillTransportPattern(data, bufferFrames, format->nBlockAlign, nextFrame, preamblesWritten);
    ThrowIfFailed(render->ReleaseBuffer(bufferFrames, 0), "Release initial render buffer");
    framesSubmitted += bufferFrames;

    std::wcout << L"Format: " << formatName << L" (" << WaveFormatText(format) << L")\n"
               << L"Buffer: " << bufferFrames << L" frames\n";
    ThrowIfFailed(client->Start(), "Start exclusive render");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const DWORD waitResult = WaitForSingleObject(renderEvent, 2'000);
        if (waitResult != WAIT_OBJECT_0) {
            client->Stop();
            CloseHandle(renderEvent);
            throw std::runtime_error("Exclusive render event timed out");
        }

        data = nullptr;
        ThrowIfFailed(render->GetBuffer(bufferFrames, &data), "Get render buffer");
        FillTransportPattern(data, bufferFrames, format->nBlockAlign,
                             nextFrame, preamblesWritten);
        ThrowIfFailed(render->ReleaseBuffer(bufferFrames, 0), "Release render buffer");
        framesSubmitted += bufferFrames;
    }

    ThrowIfFailed(client->Stop(), "Stop exclusive render");
    CloseHandle(renderEvent);
    std::wcout << L"Transport test complete\n"
               << L"Frames submitted: " << framesSubmitted << L"\n"
               << L"Marker preambles submitted: " << preamblesWritten << L"\n"
               << L"This is a transport marker, not a valid Dolby MAT payload.\n";
}

struct SpatialTone {
    AudioObjectType type;
    float frequency;
    bool emitImpulse{};
    double phase{};
    ComPtr<ISpatialAudioObject> object;
};

std::vector<SpatialTone> Spatial712Objects() {
    return {
        {AudioObjectType_FrontLeft, 220.0f},
        {AudioObjectType_FrontRight, 277.0f},
        {AudioObjectType_FrontCenter, 330.0f},
        {AudioObjectType_LowFrequency, 55.0f},
        {AudioObjectType_SideLeft, 440.0f},
        {AudioObjectType_SideRight, 554.0f},
        {AudioObjectType_BackLeft, 660.0f},
        {AudioObjectType_BackRight, 831.0f},
        {AudioObjectType_TopFrontLeft, 1000.0f},
        {AudioObjectType_TopFrontRight, 1500.0f},
    };
}

AudioObjectType SpatialObjectTypeFromName(const std::wstring& name) {
    if (name == L"fl") return AudioObjectType_FrontLeft;
    if (name == L"fr") return AudioObjectType_FrontRight;
    if (name == L"fc") return AudioObjectType_FrontCenter;
    if (name == L"lfe") return AudioObjectType_LowFrequency;
    if (name == L"sl") return AudioObjectType_SideLeft;
    if (name == L"sr") return AudioObjectType_SideRight;
    if (name == L"bl") return AudioObjectType_BackLeft;
    if (name == L"br") return AudioObjectType_BackRight;
    if (name == L"tfl") return AudioObjectType_TopFrontLeft;
    if (name == L"tfr") return AudioObjectType_TopFrontRight;
    return AudioObjectType_None;
}

void SpatialSignalTest(const double seconds, const std::wstring& filter,
                       const std::wstring& mode) {
    Endpoint endpoint = SelectEndpoint(filter);
    std::wcout << L"Spatial render endpoint: " << endpoint.name << L"\n";

    ComPtr<ISpatialAudioClient> client;
    ThrowIfFailed(endpoint.device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL, nullptr,
                                             &client),
                  "Activate ISpatialAudioClient");

    AudioObjectType nativeMask = AudioObjectType_None;
    UINT32 maxDynamicObjects = 0;
    ThrowIfFailed(client->GetNativeStaticObjectTypeMask(&nativeMask),
                  "Get native static object mask");
    ThrowIfFailed(client->GetMaxDynamicObjectCount(&maxDynamicObjects),
                  "Get maximum dynamic object count");
    std::wcout << L"Native static mask: 0x" << std::hex << std::uppercase
               << static_cast<UINT32>(nativeMask) << std::dec
               << L", dynamic objects: " << maxDynamicObjects << L"\n";

    ComPtr<IAudioFormatEnumerator> formats;
    ThrowIfFailed(client->GetSupportedAudioObjectFormatEnumerator(&formats),
                  "Get spatial object formats");
    UINT32 formatCount = 0;
    ThrowIfFailed(formats->GetCount(&formatCount), "Get spatial format count");

    std::vector<BYTE> objectFormatBytes;
    for (UINT32 index = 0; index < formatCount; ++index) {
        WAVEFORMATEX* candidate = nullptr;
        ThrowIfFailed(formats->GetFormat(index, &candidate), "Get spatial object format");
        std::wcout << L"Object format " << index << L": " << WaveFormatText(candidate) << L"\n";
        if (objectFormatBytes.empty() && IsFloatObjectFormat(candidate)) {
            const std::size_t bytes = sizeof(WAVEFORMATEX) + candidate->cbSize;
            objectFormatBytes.assign(reinterpret_cast<BYTE*>(candidate),
                                     reinterpret_cast<BYTE*>(candidate) + bytes);
        }
        CoTaskMemFree(candidate);
    }
    if (objectFormatBytes.empty()) {
        throw std::runtime_error("No 32-bit float spatial object format is available");
    }
    const auto* objectFormat =
        reinterpret_cast<const WAVEFORMATEX*>(objectFormatBytes.data());

    std::vector<SpatialTone> tones;
    bool impulseTest = false;
    bool moveDynamicObject = false;
    std::array<float, 3> dynamicPosition = {0.0f, 0.0f, 0.0f};
    if (mode == L"height") {
        tones = {
            {AudioObjectType_TopFrontLeft, 1000.0f},
            {AudioObjectType_TopFrontRight, 1500.0f},
        };
    } else if (mode == L"bed" || mode == L"712") {
        tones = Spatial712Objects();
        if (mode == L"bed") tones.resize(8);
    } else if (mode == L"dynamic") {
        if (maxDynamicObjects == 0) {
            throw std::runtime_error("The active spatial renderer exposes no dynamic objects");
        }
        moveDynamicObject = true;
        dynamicPosition = {0.0f, 0.8f, -1.0f};
        tones = {{AudioObjectType_Dynamic, 900.0f}};
    } else if (mode.starts_with(L"dynamic-")) {
        if (maxDynamicObjects == 0) {
            throw std::runtime_error("The active spatial renderer exposes no dynamic objects");
        }
        const std::wstring positionName = mode.substr(8);
        if (positionName == L"origin") dynamicPosition = {0.0f, 0.0f, 0.0f};
        else if (positionName == L"left") dynamicPosition = {-1.0f, 0.0f, 0.0f};
        else if (positionName == L"right") dynamicPosition = {1.0f, 0.0f, 0.0f};
        else if (positionName == L"above") dynamicPosition = {0.0f, 1.0f, 0.0f};
        else if (positionName == L"below") dynamicPosition = {0.0f, -1.0f, 0.0f};
        else if (positionName == L"front") dynamicPosition = {0.0f, 0.0f, -1.0f};
        else if (positionName == L"behind") dynamicPosition = {0.0f, 0.0f, 1.0f};
        else if (positionName == L"xquarter") dynamicPosition = {0.25f, 0.0f, 0.0f};
        else if (positionName == L"yhalf") dynamicPosition = {0.0f, 0.5f, 0.0f};
        else if (positionName == L"front-half") dynamicPosition = {0.0f, 0.0f, -0.5f};
        else throw std::runtime_error("Unknown fixed dynamic position");
        tones = {{AudioObjectType_Dynamic, 900.0f}};
    } else if (mode == L"silence" || mode.starts_with(L"impulse-")) {
        impulseTest = true;
        tones = Spatial712Objects();
        if (mode != L"silence") {
            const AudioObjectType impulseType = SpatialObjectTypeFromName(mode.substr(8));
            if (impulseType == AudioObjectType_None) {
                throw std::runtime_error("Unknown impulse channel");
            }
            for (SpatialTone& tone : tones) {
                tone.emitImpulse = tone.type == impulseType;
            }
        }
    } else {
        throw std::runtime_error(
            "Spatial mode must be bed, height, 712, dynamic, silence, or impulse-<channel>");
    }

    AudioObjectType staticMask = AudioObjectType_None;
    UINT32 requestedDynamicObjects = 0;
    for (const SpatialTone& tone : tones) {
        if (tone.type == AudioObjectType_Dynamic) {
            ++requestedDynamicObjects;
        } else {
            staticMask = static_cast<AudioObjectType>(static_cast<UINT32>(staticMask) |
                                                      static_cast<UINT32>(tone.type));
        }
    }
    if ((static_cast<UINT32>(staticMask) & ~static_cast<UINT32>(nativeMask)) != 0) {
        throw std::runtime_error("The requested static object layout is not native to this renderer");
    }

    HANDLE spatialEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (spatialEvent == nullptr) throw std::runtime_error("Create spatial render event failed");

    SpatialAudioObjectRenderStreamActivationParams parameters{};
    parameters.ObjectFormat = objectFormat;
    parameters.StaticObjectTypeMask = staticMask;
    parameters.MinDynamicObjectCount = requestedDynamicObjects;
    parameters.MaxDynamicObjectCount = requestedDynamicObjects;
    parameters.Category = AudioCategory_GameMedia;
    parameters.EventHandle = spatialEvent;
    parameters.NotifyObject = nullptr;

    PROPVARIANT activation;
    PropVariantInit(&activation);
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(parameters);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);

    ComPtr<ISpatialAudioObjectRenderStream> stream;
    const HRESULT activationResult = client->ActivateSpatialAudioStream(
        &activation, __uuidof(ISpatialAudioObjectRenderStream), &stream);
    if (FAILED(activationResult)) {
        CloseHandle(spatialEvent);
        ThrowIfFailed(activationResult, "Activate spatial object render stream");
    }

    for (SpatialTone& tone : tones) {
        ThrowIfFailed(stream->ActivateSpatialAudioObject(tone.type, &tone.object),
                      "Activate spatial audio object");
    }

    std::wcout << L"Spatial test mode: " << mode << L", objects: " << tones.size()
               << L", format: " << WaveFormatText(objectFormat) << L"\n";
    ThrowIfFailed(stream->Start(), "Start spatial object render stream");

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::duration<double>(seconds);
    std::uint64_t framesSubmitted = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const DWORD waitResult = WaitForSingleObject(spatialEvent, 2'000);
        if (waitResult != WAIT_OBJECT_0) {
            stream->Stop();
            CloseHandle(spatialEvent);
            throw std::runtime_error("Spatial render event timed out");
        }

        UINT32 availableDynamicObjects = 0;
        UINT32 frameCount = 0;
        ThrowIfFailed(stream->BeginUpdatingAudioObjects(&availableDynamicObjects, &frameCount),
                      "Begin updating spatial objects");

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        for (SpatialTone& tone : tones) {
            ThrowIfFailed(tone.object->SetVolume(impulseTest ? 1.0f : 0.08f),
                          "Set spatial object volume");
            BYTE* buffer = nullptr;
            UINT32 bufferBytes = 0;
            ThrowIfFailed(tone.object->GetBuffer(&buffer, &bufferBytes),
                          "Get spatial object buffer");
            const UINT32 samples = bufferBytes / sizeof(float);
            auto* output = reinterpret_cast<float*>(buffer);
            const double phaseStep =
                2.0 * 3.14159265358979323846 * tone.frequency / objectFormat->nSamplesPerSec;
            for (UINT32 sample = 0; sample < samples; ++sample) {
                if (impulseTest) {
                    constexpr std::uint64_t impulsePeriod = 960; // One impulse per MAT frame.
                    output[sample] = tone.emitImpulse &&
                                             ((framesSubmitted + sample) % impulsePeriod == 0)
                                         ? 0.5f
                                         : 0.0f;
                } else {
                    output[sample] = static_cast<float>(std::sin(tone.phase));
                    tone.phase += phaseStep;
                    if (tone.phase >= 2.0 * 3.14159265358979323846) {
                        tone.phase -= 2.0 * 3.14159265358979323846;
                    }
                }
            }
            if (tone.type == AudioObjectType_Dynamic) {
                const float x = moveDynamicObject
                                    ? static_cast<float>(std::sin(elapsed * 1.5))
                                    : dynamicPosition[0];
                ThrowIfFailed(tone.object->SetPosition(x, dynamicPosition[1], dynamicPosition[2]),
                              "Move dynamic spatial object");
            }
        }
        ThrowIfFailed(stream->EndUpdatingAudioObjects(), "End updating spatial objects");
        framesSubmitted += frameCount;
    }

    ThrowIfFailed(stream->Stop(), "Stop spatial object render stream");
    CloseHandle(spatialEvent);
    std::wcout << L"Spatial signal complete, frames submitted: " << framesSubmitted << L"\n";
}

void CaptureLoopback(const double seconds, const std::wstring& filter,
                     const std::filesystem::path& outputPath) {
    Endpoint endpoint = SelectEndpoint(filter);
    std::wcout << L"Capturing endpoint: " << endpoint.name << L"\n";

    ComPtr<IAudioClient> client;
    ThrowIfFailed(endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client),
                  "Activate IAudioClient");

    WAVEFORMATEX* format = nullptr;
    ThrowIfFailed(client->GetMixFormat(&format), "Get mix format");
    std::wcout << L"Loopback format: " << WaveFormatText(format) << L"\n";

    constexpr REFERENCE_TIME bufferDuration = 1'000'000; // 100 ms
    const HRESULT initializeResult = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufferDuration, 0, format, nullptr);
    if (FAILED(initializeResult)) {
        CoTaskMemFree(format);
        ThrowIfFailed(initializeResult, "Initialize loopback");
    }

    ComPtr<IAudioCaptureClient> capture;
    ThrowIfFailed(client->GetService(IID_PPV_ARGS(&capture)), "Get capture client");

    WaveWriter writer(outputPath, format);
    const UINT32 blockAlign = format->nBlockAlign;
    CoTaskMemFree(format);

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    ThrowIfFailed(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod), "Get device period");
    const DWORD sleepMilliseconds = std::max<DWORD>(1, static_cast<DWORD>(defaultPeriod / 20'000));

    ThrowIfFailed(client->Start(), "Start loopback");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    std::uint64_t framesCaptured = 0;
    std::uint64_t nonSilentFrames = 0;
    std::uint64_t preambles = 0;
    std::array<BYTE, 3> preambleCarry{};
    std::size_t preambleCarrySize = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        Sleep(sleepMilliseconds);
        UINT32 packetFrames = 0;
        ThrowIfFailed(capture->GetNextPacketSize(&packetFrames), "Get packet size");

        while (packetFrames != 0) {
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            ThrowIfFailed(capture->GetBuffer(&data, &packetFrames, &flags,
                                             &devicePosition, &qpcPosition),
                          "Get capture buffer");

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (!silent && data != nullptr) {
                nonSilentFrames += packetFrames;
                const auto packetBytes = static_cast<std::size_t>(packetFrames) * blockAlign;
                std::vector<BYTE> scanBytes;
                scanBytes.reserve(preambleCarrySize + packetBytes);
                scanBytes.insert(scanBytes.end(), preambleCarry.begin(),
                                 preambleCarry.begin() + preambleCarrySize);
                scanBytes.insert(scanBytes.end(), data, data + packetBytes);
                preambles += CountIec61937Preambles(scanBytes.data(), scanBytes.size());

                preambleCarrySize = std::min<std::size_t>(preambleCarry.size(), scanBytes.size());
                std::copy(scanBytes.end() - preambleCarrySize, scanBytes.end(), preambleCarry.begin());
            } else {
                preambleCarrySize = 0;
            }
            writer.Write(data, packetFrames, silent);
            framesCaptured += packetFrames;
            ThrowIfFailed(capture->ReleaseBuffer(packetFrames), "Release capture buffer");
            ThrowIfFailed(capture->GetNextPacketSize(&packetFrames), "Get packet size");
        }
    }

    ThrowIfFailed(client->Stop(), "Stop loopback");
    writer.Finalize();
    std::wcout << L"Capture written: " << outputPath.wstring() << L"\n"
               << L"Frames: " << framesCaptured << L", non-silent: " << nonSilentFrames << L"\n"
               << L"IEC 61937 Pa/Pb preambles found: " << preambles << L"\n";
}

void CaptureProcessLoopback(const double seconds, const DWORD processId,
                            const std::filesystem::path& outputPath) {
    std::wcout << L"Capturing process tree PID: " << processId << L"\n";
    ComPtr<IAudioClient> client = ActivateProcessLoopback(processId);

    WAVEFORMATEXTENSIBLE requested{};
    requested.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    requested.Format.nChannels = 8;
    requested.Format.nSamplesPerSec = 48'000;
    requested.Format.wBitsPerSample = 32;
    requested.Format.nBlockAlign = requested.Format.nChannels * sizeof(float);
    requested.Format.nAvgBytesPerSec =
        requested.Format.nSamplesPerSec * requested.Format.nBlockAlign;
    requested.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    requested.Samples.wValidBitsPerSample = 32;
    requested.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
    requested.SubFormat = kIeeeFloat;

    constexpr DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    ThrowIfFailed(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0,
                                     &requested.Format, nullptr),
                  "Initialize process loopback");
    std::wcout << L"Process-loopback format: " << WaveFormatText(&requested.Format) << L"\n";

    ComPtr<IAudioCaptureClient> capture;
    ThrowIfFailed(client->GetService(IID_PPV_ARGS(&capture)), "Get process capture client");
    WaveWriter writer(outputPath, &requested.Format);

    ThrowIfFailed(client->Start(), "Start process loopback");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    std::uint64_t framesCaptured = 0;
    std::uint64_t nonSilentFrames = 0;
    std::uint64_t preambles = 0;
    std::array<BYTE, 3> preambleCarry{};
    std::size_t preambleCarrySize = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        Sleep(5);
        UINT32 packetFrames = 0;
        ThrowIfFailed(capture->GetNextPacketSize(&packetFrames), "Get process packet size");
        while (packetFrames != 0) {
            BYTE* data = nullptr;
            DWORD bufferFlags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            ThrowIfFailed(capture->GetBuffer(&data, &packetFrames, &bufferFlags,
                                             &devicePosition, &qpcPosition),
                          "Get process capture buffer");

            const bool silent = (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (!silent && data != nullptr) {
                nonSilentFrames += packetFrames;
                const auto packetBytes = static_cast<std::size_t>(packetFrames) *
                                         requested.Format.nBlockAlign;
                std::vector<BYTE> scanBytes;
                scanBytes.reserve(preambleCarrySize + packetBytes);
                scanBytes.insert(scanBytes.end(), preambleCarry.begin(),
                                 preambleCarry.begin() + preambleCarrySize);
                scanBytes.insert(scanBytes.end(), data, data + packetBytes);
                preambles += CountIec61937Preambles(scanBytes.data(), scanBytes.size());
                preambleCarrySize = std::min<std::size_t>(preambleCarry.size(), scanBytes.size());
                std::copy(scanBytes.end() - preambleCarrySize, scanBytes.end(),
                          preambleCarry.begin());
            } else {
                preambleCarrySize = 0;
            }
            writer.Write(data, packetFrames, silent);
            framesCaptured += packetFrames;
            ThrowIfFailed(capture->ReleaseBuffer(packetFrames),
                          "Release process capture buffer");
            ThrowIfFailed(capture->GetNextPacketSize(&packetFrames),
                          "Get process packet size");
        }
    }

    ThrowIfFailed(client->Stop(), "Stop process loopback");
    writer.Finalize();
    std::wcout << L"Process capture written: " << outputPath.wstring() << L"\n"
               << L"Frames: " << framesCaptured << L", non-silent: " << nonSilentFrames << L"\n"
               << L"IEC 61937 Pa/Pb preambles found: " << preambles << L"\n";
}


} // namespace dolby
