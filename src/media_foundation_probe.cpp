#include "audio_platform.h"
#include "bridge_meter.h"
#include "commands.h"
#include "mat_capture_client.h"
#include "mat_format.h"
#include "multi_endpoint_renderer.h"
#include "speaker_layout.h"
#include "spatial_audio_sample.h"
#include "wave_io.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mfspatialaudio.h>
#include <propvarutil.h>
#include <roapi.h>
#include <SpatialAudioMetadata.h>
#include <winstring.h>
#include <appmodel.h>
#include <winrt/Windows.ApplicationModel.AppService.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dolby {
namespace {

inline constexpr GUID kDtsXEndpointMetadataFormat = {
    0x2736caba, 0x57ce, 0x43dc,
    {0x9b, 0x5d, 0xfb, 0x14, 0xbf, 0x79, 0x07, 0xac}};

inline constexpr GUID kDtsXRawSubtype = {
    0x64747378, 0x767a, 0x494d,
    {0xb4, 0x78, 0xf2, 0x9d, 0x25, 0xdc, 0x90, 0x37}};

class MediaFoundationSession {
public:
    MediaFoundationSession() {
        ThrowIfFailed(RoInitialize(RO_INIT_MULTITHREADED), "Initialize Windows Runtime");
        windowsRuntimeInitialized_ = true;
        const HRESULT result = MFStartup(MF_VERSION);
        if (FAILED(result)) {
            RoUninitialize();
            windowsRuntimeInitialized_ = false;
            ThrowIfFailed(result, "Start Media Foundation");
        }
    }
    ~MediaFoundationSession() {
        MFShutdown();
        if (windowsRuntimeInitialized_) RoUninitialize();
    }

    MediaFoundationSession(const MediaFoundationSession&) = delete;
    MediaFoundationSession& operator=(const MediaFoundationSession&) = delete;

private:
    bool windowsRuntimeInitialized_{};
};

class HString {
public:
    explicit HString(const std::wstring& value) {
        ThrowIfFailed(WindowsCreateString(value.data(), static_cast<UINT32>(value.size()), &value_),
                      "Create Windows Runtime string");
    }
    ~HString() { WindowsDeleteString(value_); }

    HSTRING Get() const { return value_; }

private:
    HSTRING value_{};
};

class LoadedModules {
public:
    ~LoadedModules() {
        for (auto module = modules_.rbegin(); module != modules_.rend(); ++module) {
            FreeLibrary(*module);
        }
    }

    LoadedModules(const LoadedModules&) = delete;
    LoadedModules& operator=(const LoadedModules&) = delete;
    LoadedModules(LoadedModules&&) noexcept = default;
    LoadedModules& operator=(LoadedModules&&) noexcept = default;
    LoadedModules() = default;

    bool Load(const std::filesystem::path& path) {
        const HMODULE module = LoadLibraryExW(
            path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) return false;
        modules_.push_back(module);
        return true;
    }

private:
    std::vector<HMODULE> modules_;
};

std::wstring ReadAllocatedString(IMFAttributes* attributes, const GUID& key);

std::vector<std::wstring> PackageFullNames(const std::wstring& family) {
    UINT32 count = 0;
    UINT32 bufferLength = 0;
    LONG result = GetPackagesByPackageFamily(
        family.c_str(), &count, nullptr, &bufferLength, nullptr);
    if (result == APPMODEL_ERROR_NO_PACKAGE) return {};
    if (result != ERROR_INSUFFICIENT_BUFFER) return {};

    std::vector<PWSTR> pointers(count);
    std::vector<wchar_t> buffer(bufferLength);
    result = GetPackagesByPackageFamily(
        family.c_str(), &count, pointers.data(), &bufferLength, buffer.data());
    if (result != ERROR_SUCCESS) return {};

    std::vector<std::wstring> names;
    names.reserve(count);
    for (UINT32 index = 0; index < count; ++index) names.emplace_back(pointers[index]);
    return names;
}

std::filesystem::path PackagePath(const std::wstring& fullName) {
    UINT32 length = 0;
    LONG result = GetPackagePathByFullName(fullName.c_str(), &length, nullptr);
    if (result != ERROR_INSUFFICIENT_BUFFER) return {};
    std::wstring path(length, L'\0');
    result = GetPackagePathByFullName(fullName.c_str(), &length, path.data());
    if (result != ERROR_SUCCESS) return {};
    path.resize(length > 0 ? length - 1 : 0);
    return path;
}

std::filesystem::path FindX64PackagePath(const std::wstring& family) {
    auto names = PackageFullNames(family);
    std::sort(names.rbegin(), names.rend());
    const auto match = std::find_if(names.begin(), names.end(), [](const std::wstring& name) {
        return Lowercase(name).find(L"_x64__") != std::wstring::npos;
    });
    return match == names.end() ? std::filesystem::path{} : PackagePath(*match);
}

LoadedModules LoadPackagedCodecDependencies(const std::filesystem::path& codecPath) {
    LoadedModules modules;
    const auto runtimePath = FindX64PackagePath(L"Microsoft.VCLibs.140.00_8wekyb3d8bbwe");
    if (runtimePath.empty()) {
        std::wcout << L"    Microsoft.VCLibs.140.00 x64 package was not found.\n";
        return modules;
    }

    for (const wchar_t* name : {
             L"vcruntime140_app.dll", L"vcruntime140_1_app.dll",
             L"msvcp140_app.dll", L"vccorlib140_app.dll"}) {
        const auto path = runtimePath / name;
        if (!modules.Load(path)) {
            std::wcout << L"    Could not preload " << path << L": Win32 "
                       << GetLastError() << L"\n";
        }
    }
    if (!codecPath.empty()) {
        if (modules.Load(codecPath)) {
            std::wcout << L"    Preloaded packaged codec: " << codecPath << L"\n";
        } else {
            std::wcout << L"    Could not preload codec " << codecPath << L": Win32 "
                       << GetLastError() << L"\n";
        }
    }
    return modules;
}

struct ActivatedDecoder {
    LoadedModules modules;
    ComPtr<IMFActivate> activation;
    ComPtr<IMFTransform> transform;
};

class FieldOfUseUnlockProbe final : public IMFFieldOfUseMFTUnlock {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (interfaceId == IID_IUnknown || interfaceId == IID_IMFFieldOfUseMFTUnlock) {
            *object = static_cast<IMFFieldOfUseMFTUnlock*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Unlock(IUnknown* transform) override {
        ++calls_;
        ComPtr<IMFTransform> mediaTransform;
        const HRESULT query = transform != nullptr
            ? transform->QueryInterface(IID_PPV_ARGS(&mediaTransform))
            : E_POINTER;
        std::wcout << L"    IMFFieldOfUseMFTUnlock::Unlock invoked; transform="
                   << HResultText(query) << L"\n";
        // DTS does not publish a field-of-use handshake. Do not report an unlock without one.
        return MF_E_UNAUTHORIZED;
    }

    ULONG Calls() const { return calls_.load(); }

private:
    ~FieldOfUseUnlockProbe() = default;

    std::atomic<ULONG> references_{1};
    std::atomic<ULONG> calls_{};
};

std::vector<ComPtr<IMFActivate>> EnumerateAudioDecoders(const UINT32 flags) {
    IMFActivate** rawActivations = nullptr;
    UINT32 count = 0;
    ThrowIfFailed(MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, flags, nullptr, nullptr,
                            &rawActivations, &count),
                  "Enumerate Media Foundation audio decoders");
    std::vector<ComPtr<IMFActivate>> activations;
    activations.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
        activations.emplace_back(rawActivations[index]);
    }
    CoTaskMemFree(rawActivations);
    return activations;
}

ComPtr<IMFActivate> FindAudioDecoder(const std::vector<ComPtr<IMFActivate>>& activations,
                                    const std::wstring& requestedName) {
    for (const auto& activation : activations) {
        const std::wstring name = ReadAllocatedString(
            activation.Get(), MFT_FRIENDLY_NAME_Attribute);
        if (Lowercase(name) == Lowercase(requestedName)) return activation;
    }
    return {};
}

HRESULT ActivateDecoderObject(IMFActivate* activation, const std::filesystem::path& codecPath,
                              LoadedModules& fallbackModules, IMFTransform** transform) {
    HRESULT result = activation->ActivateObject(IID_PPV_ARGS(transform));
    if (result != HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) || codecPath.empty()) return result;

    std::wcout << L"    Package activation could not resolve codec dependencies; "
                  L"using the desktop preload fallback.\n";
    fallbackModules = LoadPackagedCodecDependencies(codecPath);
    return activation->ActivateObject(IID_PPV_ARGS(transform));
}

ActivatedDecoder ActivateAudioDecoder(const std::wstring& requestedName,
                                      const GUID* inputSubtype = nullptr,
                                      const GUID* outputSubtype = nullptr) {
    IMFActivate** rawActivations = nullptr;
    UINT32 count = 0;
    MFT_REGISTER_TYPE_INFO inputInfo{};
    MFT_REGISTER_TYPE_INFO outputInfo{};
    if (inputSubtype != nullptr) inputInfo = {MFMediaType_Audio, *inputSubtype};
    if (outputSubtype != nullptr) outputInfo = {MFMediaType_Audio, *outputSubtype};
    ThrowIfFailed(MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_ALL,
                            inputSubtype != nullptr ? &inputInfo : nullptr,
                            outputSubtype != nullptr ? &outputInfo : nullptr,
                            &rawActivations, &count),
                  "Enumerate Media Foundation audio decoders");
    std::vector<ComPtr<IMFActivate>> activations;
    activations.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
        activations.emplace_back(rawActivations[index]);
    }
    CoTaskMemFree(rawActivations);

    static constexpr GUID kPackagedCodecPath = {
        0x7347c815, 0x79fc, 0x4ad9,
        {0x87, 0x7d, 0xac, 0xdf, 0x5f, 0x46, 0x68, 0x5e}};
    for (const auto& activation : activations) {
        const std::wstring name = ReadAllocatedString(activation.Get(), MFT_FRIENDLY_NAME_Attribute);
        if (Lowercase(name) != Lowercase(requestedName)) continue;

        ActivatedDecoder result;
        result.activation = activation;
        const auto codecPath = ReadAllocatedString(activation.Get(), kPackagedCodecPath);
        ThrowIfFailed(ActivateDecoderObject(activation.Get(), codecPath, result.modules,
                                            &result.transform),
                      "Activate audio decoder");
        return result;
    }
    throw std::runtime_error("Requested Media Foundation audio decoder was not found");
}

std::vector<std::vector<BYTE>> LoadDtsXFrames(const std::filesystem::path& inputPath,
                                              const std::size_t maxBursts) {
    const WaveImage image = ReadWaveImage(inputPath);
    const BYTE* data = image.bytes.data() + image.dataOffset;
    const std::size_t dataBytes = image.dataBytes;
    std::vector<std::vector<BYTE>> frames;
    for (std::size_t offset = 0; offset + 20 <= dataBytes && frames.size() < maxBursts;
         ++offset) {
        const bool little = data[offset] == 0x72 && data[offset + 1] == 0xf8 &&
                            data[offset + 2] == 0x1f && data[offset + 3] == 0x4e;
        const bool swapped = data[offset] == 0xf8 && data[offset + 1] == 0x72 &&
                             data[offset + 2] == 0x4e && data[offset + 3] == 0x1f;
        if (!little && !swapped) continue;
        const auto readWord = [&](const std::size_t wordOffset) {
            return swapped
                       ? static_cast<std::uint16_t>((data[wordOffset] << 8) |
                                                    data[wordOffset + 1])
                       : ReadLittleUint16(data + wordOffset);
        };
        const std::uint16_t pc = readWord(offset + 4);
        const std::size_t payloadBytes = readWord(offset + 6);
        if ((pc & 0x1fU) != 0x11 || payloadBytes < 12 ||
            offset + 8 + payloadBytes > dataBytes) {
            continue;
        }
        const auto logical = UnswapMatTransportWords(data + offset + 8, payloadBytes);
        constexpr std::array<BYTE, 10> wrapper = {
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xfe};
        if (!std::equal(wrapper.begin(), wrapper.end(), logical.begin())) continue;
        const std::size_t frameBytes =
            (static_cast<std::size_t>(logical[10]) << 8) | logical[11];
        if (frameBytes == 0 || frameBytes + 12 > logical.size()) continue;
        frames.emplace_back(logical.begin() + 12, logical.begin() + 12 + frameBytes);
        offset += payloadBytes + 7;
    }
    if (frames.empty()) throw std::runtime_error("No valid DTS:X E1 frames were found");
    return frames;
}

std::wstring AudioObjectTypeName(const AudioObjectType type) {
    switch (type) {
    case AudioObjectType_Dynamic: return L"Dynamic";
    case AudioObjectType_FrontLeft: return L"FL";
    case AudioObjectType_FrontRight: return L"FR";
    case AudioObjectType_FrontCenter: return L"FC";
    case AudioObjectType_LowFrequency: return L"LFE";
    case AudioObjectType_SideLeft: return L"SL";
    case AudioObjectType_SideRight: return L"SR";
    case AudioObjectType_BackLeft: return L"BL";
    case AudioObjectType_BackRight: return L"BR";
    case AudioObjectType_TopFrontLeft: return L"TFL";
    case AudioObjectType_TopFrontRight: return L"TFR";
    case AudioObjectType_TopBackLeft: return L"TBL";
    case AudioObjectType_TopBackRight: return L"TBR";
    default: return L"0x" + std::to_wstring(static_cast<std::uint32_t>(type));
    }
}

void PrintSpatialSample(IMFSample* sample, const std::size_t sampleIndex) {
    ComPtr<IMFSpatialAudioSample> spatial;
    const HRESULT query = sample->QueryInterface(IID_PPV_ARGS(&spatial));
    if (FAILED(query)) {
        std::wcout << L"Output sample " << sampleIndex << L" is not IMFSpatialAudioSample: "
                   << HResultText(query) << L"\n";
        return;
    }
    DWORD objectCount = 0;
    ThrowIfFailed(spatial->GetObjectCount(&objectCount), "Get decoded spatial object count");
    std::wcout << L"Output sample " << sampleIndex << L": " << objectCount
               << L" spatial objects\n";
    for (DWORD index = 0; index < objectCount; ++index) {
        ComPtr<IMFSpatialAudioObjectBuffer> object;
        ThrowIfFailed(spatial->GetSpatialAudioObjectByIndex(index, &object),
                      "Get decoded spatial object");
        UINT32 id = 0;
        AudioObjectType type = AudioObjectType_None;
        DWORD bytes = 0;
        object->GetID(&id);
        object->GetType(&type);
        object->GetCurrentLength(&bytes);

        double rms = 0.0;
        BYTE* raw = nullptr;
        DWORD maximum = 0;
        DWORD current = 0;
        if (SUCCEEDED(object->Lock(&raw, &maximum, &current)) && raw != nullptr) {
            const auto* values = reinterpret_cast<const float*>(raw);
            const std::size_t valueCount = current / sizeof(float);
            long double squareSum = 0.0;
            for (std::size_t valueIndex = 0; valueIndex < valueCount; ++valueIndex) {
                squareSum += static_cast<long double>(values[valueIndex]) * values[valueIndex];
            }
            if (valueCount != 0) rms = std::sqrt(static_cast<double>(squareSum / valueCount));
            object->Unlock();
        }

        ComPtr<ISpatialAudioMetadataItems> metadata;
        SpatialAudioMetadataItemsInfo metadataInfo{};
        const HRESULT metadataResult = object->GetMetadataItems(&metadata);
        if (SUCCEEDED(metadataResult) && metadata) metadata->GetInfo(&metadataInfo);
        std::wcout << L"  [" << index << L"] id=" << id << L", type="
                   << AudioObjectTypeName(type) << L", audio=" << bytes << L" bytes, rms="
                   << rms << L", metadata-items=" << metadataInfo.ItemCount
                   << L", metadata-frames=" << metadataInfo.FrameCount << L"\n";
    }
}

double ReadPcmValue(const BYTE* data, const UINT32 bitsPerSample, const GUID& subtype) {
    if (IsEqualGUID(subtype, MFAudioFormat_Float) && bitsPerSample == 32) {
        float value = 0.0F;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    if (bitsPerSample == 16) {
        std::int16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<double>(value) / 32'768.0;
    }
    if (bitsPerSample == 24) {
        std::int32_t value = static_cast<std::int32_t>(data[0]) |
                             (static_cast<std::int32_t>(data[1]) << 8) |
                             (static_cast<std::int32_t>(data[2]) << 16);
        if ((value & 0x0080'0000) != 0) value |= static_cast<std::int32_t>(0xff00'0000);
        return static_cast<double>(value) / 8'388'608.0;
    }
    if (bitsPerSample == 32) {
        std::int32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<double>(value) / 2'147'483'648.0;
    }
    return 0.0;
}

void PrintPcmSample(IMFSample* sample, IMFMediaType* mediaType,
                    const std::size_t sampleIndex, WaveWriter* writer) {
    ComPtr<IMFMediaBuffer> buffer;
    ThrowIfFailed(sample->ConvertToContiguousBuffer(&buffer),
                  "Get contiguous DTS:X PCM output");

    UINT32 channels = 0;
    UINT32 bitsPerSample = 0;
    UINT32 blockAlignment = 0;
    GUID subtype{};
    ThrowIfFailed(mediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels),
                  "Get DTS:X PCM channel count");
    ThrowIfFailed(mediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample),
                  "Get DTS:X PCM bit depth");
    ThrowIfFailed(mediaType->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &blockAlignment),
                  "Get DTS:X PCM block alignment");
    ThrowIfFailed(mediaType->GetGUID(MF_MT_SUBTYPE, &subtype),
                  "Get DTS:X PCM subtype");

    BYTE* raw = nullptr;
    DWORD maximum = 0;
    DWORD current = 0;
    ThrowIfFailed(buffer->Lock(&raw, &maximum, &current), "Lock DTS:X PCM output");
    const UINT32 bytesPerValue = bitsPerSample / 8;
    const std::size_t frameCount = blockAlignment == 0 ? 0 : current / blockAlignment;
    std::vector<long double> squareSums(channels, 0.0);
    if (bytesPerValue != 0 && frameCount != 0) {
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            for (UINT32 channel = 0; channel < channels; ++channel) {
                const BYTE* value = raw + frame * blockAlignment + channel * bytesPerValue;
                const double normalized = ReadPcmValue(value, bitsPerSample, subtype);
                squareSums[channel] +=
                    static_cast<long double>(normalized) * normalized;
            }
        }
    }
    if (writer != nullptr && frameCount != 0) {
        writer->Write(raw, static_cast<UINT32>(frameCount), false);
    }
    buffer->Unlock();

    if (sampleIndex < 4) {
        std::wcout << L"Output sample " << sampleIndex << L": " << frameCount
                   << L" PCM frames, channel RMS=";
        for (UINT32 channel = 0; channel < channels; ++channel) {
            const double rms = frameCount == 0
                ? 0.0
                : std::sqrt(static_cast<double>(squareSums[channel] / frameCount));
            if (channel != 0) std::wcout << L", ";
            std::wcout << rms;
        }
        std::wcout << L"\n";
    }
}

ComPtr<IMFMediaType> ConfigureDtsXInputType(IMFTransform* transform,
                                            IMFMediaType* advertisedType,
                                            const UINT32 measuredAverageBytes) {
    ComPtr<IMFMediaType> minimalType;
    ThrowIfFailed(MFCreateMediaType(&minimalType), "Create minimal DTS:X input type");
    ThrowIfFailed(advertisedType->CopyAllItems(minimalType.Get()),
                  "Copy minimal DTS:X input type");
    const HRESULT minimalResult =
        transform->SetInputType(0, minimalType.Get(), MFT_SET_TYPE_TEST_ONLY);
    std::wcout << L"  advertised minimal type: " << HResultText(minimalResult) << L"\n";
    if (SUCCEEDED(minimalResult)) return minimalType;

    const std::array<GUID, 3> exactSubtypes = {{
        MFAudioFormat_DTS_HD,
        {0x64747378, 0x767a, 0x494d, {0xb4, 0x78, 0xf2, 0x9d, 0x25, 0xdc, 0x90, 0x37}},
        {0x64747368, 0x767a, 0x494d, {0xb4, 0x78, 0xf2, 0x9d, 0x25, 0xdc, 0x90, 0x37}},
    }};
    const std::array<UINT32, 4> averageRates = {
        measuredAverageBytes, 96'058, 196'933, 768'000};
    for (const GUID& subtype : exactSubtypes) {
        for (const UINT32 channels : {12U, 8U, 6U}) {
            for (const UINT32 averageBytes : averageRates) {
                ComPtr<IMFMediaType> candidate;
                ThrowIfFailed(MFCreateMediaType(&candidate),
                              "Create DTS-HD source-style input type");
                candidate->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                candidate->SetGUID(MF_MT_SUBTYPE, subtype);
                candidate->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
                candidate->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48'000);
                candidate->SetDouble(MF_MT_AUDIO_FLOAT_SAMPLES_PER_SECOND, 48'000.0);
                candidate->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 24);
                candidate->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, averageBytes);
                candidate->SetUINT32(MF_MT_COMPRESSED, TRUE);
                const HRESULT result =
                    transform->SetInputType(0, candidate.Get(), MFT_SET_TYPE_TEST_ONLY);
                if (SUCCEEDED(result)) {
                    std::wcout << L"  accepted source-style subtype=" << GuidText(subtype)
                               << L", channels=" << channels << L", avg=" << averageBytes
                               << L"\n";
                    return candidate;
                }
            }
        }
    }
    std::wcout << L"  source-style DTS-HD profiles were rejected\n";

    struct Profile {
        UINT32 channels;
        UINT32 bits;
        UINT32 blockAlignment;
        UINT32 averageBytes;
    };
    const std::array<Profile, 15> profiles = {{
        {1, 0, 0, 0}, {2, 0, 0, 0}, {6, 0, 0, 0}, {8, 0, 0, 0}, {12, 0, 0, 0},
        {1, 16, 1, 192'000}, {2, 16, 1, 192'000}, {6, 16, 1, 192'000},
        {8, 16, 1, 192'000}, {12, 16, 1, 192'000},
        {1, 32, 4, 768'000}, {2, 32, 8, 768'000}, {6, 32, 24, 768'000},
        {8, 32, 32, 768'000}, {12, 32, 48, 768'000},
    }};
    const std::array<GUID, 11> subtypes = {{
        {0x64747378, 0x767a, 0x494d, {0xb4, 0x78, 0xf2, 0x9d, 0x25, 0xdc, 0x90, 0x37}},
        {0x64747368, 0x767a, 0x494d, {0xb4, 0x78, 0xf2, 0x9d, 0x25, 0xdc, 0x90, 0x37}},
        {0x64747363, 0x767a, 0x494d, {0xb4, 0x78, 0xf2, 0x9d, 0x25, 0xdc, 0x90, 0x37}},
        {0x64747365, 0x767a, 0x494d, {0xb4, 0x78, 0xf2, 0x9d, 0x25, 0xdc, 0x90, 0x37}},
        {0x6474736c, 0x767a, 0x494d, {0xb4, 0x78, 0xf2, 0x9d, 0x25, 0xdc, 0x90, 0x37}},
        MFAudioFormat_DTS_HD,
        MFAudioFormat_DTS_RAW,
        MFAudioFormat_DTS_XLL,
        MFAudioFormat_DTS_LBR,
        MFAudioFormat_DTS_UHD,
        kDtsHd,
    }};
    for (const GUID& subtype : subtypes) {
        HRESULT lastResult = MF_E_INVALIDMEDIATYPE;
        for (const Profile& profile : profiles) {
            ComPtr<IMFMediaType> candidate;
            ThrowIfFailed(MFCreateMediaType(&candidate), "Create DTS:X candidate input type");
            ThrowIfFailed(advertisedType->CopyAllItems(candidate.Get()),
                          "Copy DTS:X candidate input type");
            candidate->SetGUID(MF_MT_SUBTYPE, subtype);
            candidate->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48'000);
            candidate->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, profile.channels);
            DWORD channelMask = 0;
            switch (profile.channels) {
            case 1: channelMask = SPEAKER_FRONT_CENTER; break;
            case 2: channelMask = KSAUDIO_SPEAKER_STEREO; break;
            case 6: channelMask = KSAUDIO_SPEAKER_5POINT1; break;
            case 8: channelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND; break;
            case 12: channelMask = 0x0002d63f; break; // 7.1.4
            default: break;
            }
            candidate->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, channelMask);
            candidate->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            if (profile.bits != 0) {
                candidate->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, profile.bits);
                candidate->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, profile.blockAlignment);
                candidate->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, profile.averageBytes);
            }
            lastResult = transform->SetInputType(0, candidate.Get(), MFT_SET_TYPE_TEST_ONLY);
            if (SUCCEEDED(lastResult)) {
                std::wcout << L"  accepted subtype=" << GuidText(subtype)
                           << L", channels=" << profile.channels << L", bits="
                           << profile.bits << L", block=" << profile.blockAlignment
                           << L", avg=" << profile.averageBytes << L"\n";
                return candidate;
            }
        }
        std::wcout << L"  rejected subtype=" << GuidText(subtype)
                   << L": " << HResultText(lastResult) << L"\n";
    }
    return {};
}

std::wstring ReadAllocatedString(IMFAttributes* attributes, const GUID& key) {
    wchar_t* raw = nullptr;
    UINT32 length = 0;
    const HRESULT result = attributes->GetAllocatedString(key, &raw, &length);
    if (FAILED(result)) return {};
    std::wstring value(raw, length);
    CoTaskMemFree(raw);
    return value;
}

std::wstring PropVariantText(const PROPVARIANT& value) {
    switch (value.vt) {
    case VT_EMPTY: return L"(empty)";
    case VT_UI4: return std::to_wstring(value.ulVal);
    case VT_UI8: return std::to_wstring(value.uhVal.QuadPart);
    case VT_I4: return std::to_wstring(value.lVal);
    case VT_LPWSTR: return value.pwszVal != nullptr ? value.pwszVal : L"(null)";
    case VT_BSTR: return value.bstrVal != nullptr ? value.bstrVal : L"(null)";
    case VT_CLSID: return value.puuid != nullptr ? GuidText(*value.puuid) : L"(null GUID)";
    case VT_UNKNOWN: return L"IUnknown";
    case VT_BLOB: return L"blob, " + std::to_wstring(value.blob.cbSize) + L" bytes";
    default:
        if (value.vt == (VT_VECTOR | VT_UI1)) {
            return L"byte vector, " + std::to_wstring(value.caub.cElems) + L" bytes";
        }
        return L"variant type " + std::to_wstring(value.vt);
    }
}

void PrintAttributes(IMFAttributes* attributes) {
    UINT32 count = 0;
    if (FAILED(attributes->GetCount(&count))) return;
    std::wcout << L"    Activation attributes (" << count << L"):\n";
    for (UINT32 index = 0; index < count; ++index) {
        GUID key{};
        PROPVARIANT value;
        PropVariantInit(&value);
        const HRESULT result = attributes->GetItemByIndex(index, &key, &value);
        if (SUCCEEDED(result)) {
            std::wcout << L"      " << GuidText(key) << L" = " << PropVariantText(value) << L"\n";
        }
        PropVariantClear(&value);
    }
}

std::wstring MediaTypeText(IMFMediaType* type) {
    GUID major{};
    GUID subtype{};
    type->GetGUID(MF_MT_MAJOR_TYPE, &major);
    type->GetGUID(MF_MT_SUBTYPE, &subtype);

    std::wstring text = L"major=" + GuidText(major) + L", subtype=" + GuidText(subtype);
    UINT32 value = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &value))) {
        text += L", channels=" + std::to_wstring(value);
    }
    if (SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &value))) {
        text += L", rate=" + std::to_wstring(value);
    }
    if (SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &value))) {
        text += L", bits=" + std::to_wstring(value);
    }
    if (SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &value))) {
        wchar_t mask[16]{};
        swprintf_s(mask, L"0x%08X", value);
        text += L", mask=" + std::wstring(mask);
    }
    if (SUCCEEDED(type->GetUINT32(MF_MT_SPATIAL_AUDIO_MAX_DYNAMIC_OBJECTS, &value))) {
        text += L", max-dynamic=" + std::to_wstring(value);
    }
    if (SUCCEEDED(type->GetUINT32(MF_MT_SPATIAL_AUDIO_OBJECT_METADATA_LENGTH, &value))) {
        text += L", metadata-bytes=" + std::to_wstring(value);
    }
    if (SUCCEEDED(type->GetUINT32(MF_MT_SPATIAL_AUDIO_MAX_METADATA_ITEMS, &value))) {
        text += L", max-metadata-items=" + std::to_wstring(value);
    }
    if (SUCCEEDED(type->GetUINT32(MF_MT_SPATIAL_AUDIO_MIN_METADATA_ITEM_OFFSET_SPACING, &value))) {
        text += L", metadata-spacing=" + std::to_wstring(value);
    }
    GUID metadataFormat{};
    if (SUCCEEDED(type->GetGUID(MF_MT_SPATIAL_AUDIO_OBJECT_METADATA_FORMAT_ID,
                                &metadataFormat))) {
        text += L", metadata-format=" + GuidText(metadataFormat);
    }
    return text;
}

std::vector<DWORD> StreamIds(IMFTransform* transform, const bool input) {
    DWORD inputCount = 0;
    DWORD outputCount = 0;
    ThrowIfFailed(transform->GetStreamCount(&inputCount, &outputCount), "Get decoder stream count");
    const DWORD count = input ? inputCount : outputCount;
    std::vector<DWORD> inputIds(inputCount);
    std::vector<DWORD> outputIds(outputCount);
    const HRESULT result = transform->GetStreamIDs(
        inputCount, inputIds.data(), outputCount, outputIds.data());
    if (result == E_NOTIMPL) {
        std::vector<DWORD> sequential(count);
        for (DWORD index = 0; index < count; ++index) sequential[index] = index;
        return sequential;
    }
    ThrowIfFailed(result, "Get decoder stream IDs");
    return input ? inputIds : outputIds;
}

void PrintAvailableTypes(IMFTransform* transform) {
    for (const bool input : {true, false}) {
        const auto streamIds = StreamIds(transform, input);
        std::wcout << (input ? L"    Input types:\n" : L"    Output types:\n");
        for (const DWORD streamId : streamIds) {
            for (DWORD typeIndex = 0;; ++typeIndex) {
                ComPtr<IMFMediaType> type;
                const HRESULT result = input
                    ? transform->GetInputAvailableType(streamId, typeIndex, &type)
                    : transform->GetOutputAvailableType(streamId, typeIndex, &type);
                if (result == MF_E_NO_MORE_TYPES) break;
                if (FAILED(result)) {
                    std::wcout << L"      stream " << streamId << L": error "
                               << HResultText(result) << L"\n";
                    break;
                }
                std::wcout << L"      stream " << streamId << L" [" << typeIndex << L"] "
                           << MediaTypeText(type.Get()) << L"\n";
            }
        }
    }
}

bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    return needle.empty() || Lowercase(haystack).find(Lowercase(needle)) != std::wstring::npos;
}

void PrintInspectableClass(IInspectable* inspectable) {
    HSTRING runtimeClass{};
    const HRESULT result = inspectable->GetRuntimeClassName(&runtimeClass);
    if (FAILED(result)) {
        std::wcout << L"Runtime class: unavailable (" << HResultText(result) << L")\n";
        return;
    }
    UINT32 length = 0;
    const wchar_t* value = WindowsGetStringRawBuffer(runtimeClass, &length);
    std::wcout << L"Runtime class: " << std::wstring(value, length) << L"\n";
    WindowsDeleteString(runtimeClass);
}

class DtsXCarrierFramer {
public:
    std::vector<std::vector<BYTE>> Push(const BYTE* data, const std::size_t bytes) {
        buffer_.insert(buffer_.end(), data, data + bytes);
        std::vector<std::vector<BYTE>> frames;
        std::size_t offset = 0;
        std::uint64_t skippedBytes = 0;
        while (offset + 8 <= buffer_.size()) {
            const bool little = buffer_[offset] == 0x72 && buffer_[offset + 1] == 0xf8 &&
                                buffer_[offset + 2] == 0x1f && buffer_[offset + 3] == 0x4e;
            const bool swapped = buffer_[offset] == 0xf8 && buffer_[offset + 1] == 0x72 &&
                                 buffer_[offset + 2] == 0x4e && buffer_[offset + 3] == 0x1f;
            if (!little && !swapped) {
                ++skippedBytes;
                ++offset;
                continue;
            }
            const auto readWord = [&](const std::size_t wordOffset) {
                return swapped
                    ? static_cast<std::uint16_t>((buffer_[wordOffset] << 8) |
                                                 buffer_[wordOffset + 1])
                    : ReadLittleUint16(buffer_.data() + wordOffset);
            };
            const std::uint16_t dataType = readWord(offset + 4);
            const std::size_t payloadBytes = readWord(offset + 6);
            if ((dataType & 0x1fU) != 0x11 || payloadBytes < 12) {
                ++malformedBursts_;
                ++skippedBytes;
                ++offset;
                continue;
            }
            if (offset + 8 + payloadBytes > buffer_.size()) break;

            const auto logical = UnswapMatTransportWords(
                buffer_.data() + offset + 8, payloadBytes);
            constexpr std::array<BYTE, 10> wrapper = {
                0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xfe};
            if (!std::equal(wrapper.begin(), wrapper.end(), logical.begin())) {
                ++malformedBursts_;
                skippedBytes += 8 + payloadBytes;
                offset += 8 + payloadBytes;
                continue;
            }
            const std::size_t frameBytes =
                (static_cast<std::size_t>(logical[10]) << 8) | logical[11];
            if (frameBytes == 0 || frameBytes + 12 > logical.size()) {
                ++malformedBursts_;
                skippedBytes += 8 + payloadBytes;
                offset += 8 + payloadBytes;
                continue;
            }
            frames.emplace_back(logical.begin() + 12,
                                logical.begin() + 12 + frameBytes);
            ++bursts_;
            offset += 8 + payloadBytes;
        }
        skippedBytes_ += skippedBytes;
        buffer_.erase(buffer_.begin(), buffer_.begin() + offset);
        return frames;
    }

    void Reset() { buffer_.clear(); }
    std::uint64_t Bursts() const { return bursts_; }
    std::uint64_t MalformedBursts() const { return malformedBursts_; }
    std::uint64_t SkippedBytes() const { return skippedBytes_; }
    std::size_t BufferedBytes() const { return buffer_.size(); }

private:
    std::vector<BYTE> buffer_;
    std::uint64_t bursts_{};
    std::uint64_t malformedBursts_{};
    std::uint64_t skippedBytes_{};
};

struct DtsXSpatialDecodeStats {
    std::uint64_t inputFrames{};
    std::uint64_t outputSamples{};
    std::uint64_t pcmFrames{};
    std::uint64_t clippedSamples{};
    std::uint64_t unmappedObjects{};
};

class DtsXSpatialDecoder {
public:
    explicit DtsXSpatialDecoder(const SpeakerLayout& layout) : layout_(layout) {
        decoder_ = ActivateAudioDecoder(
            L"DTSXDecoder", &kDtsXRawSubtype, &MFAudioFormat_Float_SpatialObjects);
        const Endpoint endpoint = SelectEndpoint(L"SinkDescription Sample");

        PROPVARIANT metadataActivation;
        PropVariantInit(&metadataActivation);
        metadataActivation.vt = VT_CLSID;
        metadataActivation.puuid = const_cast<GUID*>(&kDtsXEndpointMetadataFormat);
        ThrowIfFailed(endpoint.device->Activate(__uuidof(ISpatialAudioMetadataClient), CLSCTX_ALL,
                                                &metadataActivation, &metadataClient_),
                      "Activate live DTS:X metadata client");

        ComPtr<IMFAttributes> attributes;
        ThrowIfFailed(decoder_.transform->GetAttributes(&attributes),
                      "Get live DTS:X transform attributes");
        ThrowIfFailed(attributes->SetString(MFT_AUDIO_DECODER_AUDIO_ENDPOINT_ID,
                                            endpoint.id.c_str()),
                      "Set live DTS:X endpoint ID");
        ThrowIfFailed(attributes->SetUnknown(MFT_AUDIO_DECODER_SPATIAL_METADATA_CLIENT,
                                             metadataClient_.Get()),
                      "Set live DTS:X metadata client");

        ComPtr<IMFMediaType> advertisedInput;
        ThrowIfFailed(decoder_.transform->GetInputAvailableType(0, 0, &advertisedInput),
                      "Get live DTS:X input type");
        outputType_ = FindOutputType();
        ThrowIfFailed(decoder_.transform->SetOutputType(0, outputType_.Get(), 0),
                      "Set live DTS:X spatial output type");
        const ComPtr<IMFMediaType> inputType = ConfigureDtsXInputType(
            decoder_.transform.Get(), advertisedInput.Get(), 196'933);
        if (!inputType) throw std::runtime_error("No live DTS:X input profile was accepted");
        ThrowIfFailed(decoder_.transform->SetInputType(0, inputType.Get(), 0),
                      "Set live DTS:X input type");
        ThrowIfFailed(decoder_.transform->GetOutputStreamInfo(0, &outputInfo_),
                      "Get live DTS:X output stream information");
        if ((outputInfo_.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                    MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0) {
            throw std::runtime_error("Unexpected DTS:X sample allocation mode");
        }
        ThrowIfFailed(outputType_->GetUINT32(MF_MT_SPATIAL_AUDIO_MAX_DYNAMIC_OBJECTS,
                                             &objectCount_),
                      "Get live DTS:X object count");
        ThrowIfFailed(outputType_->GetUINT32(MF_MT_SPATIAL_AUDIO_MAX_METADATA_ITEMS,
                                             &maxMetadataItems_),
                      "Get live DTS:X metadata item count");

        decoder_.transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        ThrowIfFailed(decoder_.transform->ProcessMessage(
                          MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
                      "Begin live DTS:X streaming");
        ThrowIfFailed(decoder_.transform->ProcessMessage(
                          MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
                      "Start live DTS:X stream");
        streaming_ = true;
    }

    ~DtsXSpatialDecoder() {
        if (streaming_) {
            decoder_.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
    }

    DtsXSpatialDecoder(const DtsXSpatialDecoder&) = delete;
    DtsXSpatialDecoder& operator=(const DtsXSpatialDecoder&) = delete;

    std::vector<std::int16_t> Push(const std::vector<BYTE>& frame) {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        ThrowIfFailed(MFCreateSample(&sample), "Create live DTS:X input sample");
        ThrowIfFailed(MFCreateMemoryBuffer(static_cast<DWORD>(frame.size()), &buffer),
                      "Create live DTS:X input buffer");
        BYTE* destination = nullptr;
        DWORD maximum = 0;
        ThrowIfFailed(buffer->Lock(&destination, &maximum, nullptr),
                      "Lock live DTS:X input buffer");
        std::memcpy(destination, frame.data(), frame.size());
        buffer->Unlock();
        ThrowIfFailed(buffer->SetCurrentLength(static_cast<DWORD>(frame.size())),
                      "Set live DTS:X input length");
        ThrowIfFailed(sample->AddBuffer(buffer.Get()), "Attach live DTS:X input buffer");
        ThrowIfFailed(sample->SetSampleTime(sampleTime_), "Set live DTS:X sample time");
        ThrowIfFailed(sample->SetSampleDuration(kFrameDuration),
                      "Set live DTS:X sample duration");
        if (discontinuity_) {
            sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
            discontinuity_ = false;
        }
        sampleTime_ += kFrameDuration;

        std::vector<std::int16_t> output;
        HRESULT result = decoder_.transform->ProcessInput(0, sample.Get(), 0);
        if (result == MF_E_NOTACCEPTING) {
            output = Pull();
            result = decoder_.transform->ProcessInput(0, sample.Get(), 0);
        }
        ThrowIfFailed(result, "Submit live DTS:X frame");
        ++stats_.inputFrames;
        auto more = Pull();
        output.insert(output.end(), more.begin(), more.end());
        return output;
    }

    std::vector<std::int16_t> Drain() {
        ThrowIfFailed(decoder_.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0),
                      "End live DTS:X stream");
        ThrowIfFailed(decoder_.transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0),
                      "Drain live DTS:X decoder");
        auto output = Pull();
        decoder_.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        streaming_ = false;
        return output;
    }

    void Reset() {
        decoder_.transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        decoder_.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        sampleTime_ = 0;
        discontinuity_ = true;
    }

    const DtsXSpatialDecodeStats& Stats() const { return stats_; }

private:
    static constexpr LONGLONG kFrameDuration = (10'000'000LL * 512) / 48'000;

    ComPtr<IMFMediaType> FindOutputType() {
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> candidate;
            const HRESULT result = decoder_.transform->GetOutputAvailableType(0, index,
                                                                               &candidate);
            if (result == MF_E_NO_MORE_TYPES) break;
            ThrowIfFailed(result, "Enumerate live DTS:X output types");
            GUID subtype{};
            GUID metadataFormat{};
            if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                IsEqualGUID(subtype, MFAudioFormat_Float_SpatialObjects) &&
                SUCCEEDED(candidate->GetGUID(MF_MT_SPATIAL_AUDIO_OBJECT_METADATA_FORMAT_ID,
                                             &metadataFormat)) &&
                IsEqualGUID(metadataFormat, kDtsXEndpointMetadataFormat)) {
                return candidate;
            }
        }
        throw std::runtime_error("DTS:X endpoint metadata output was not found");
    }

    std::vector<std::int16_t> Pull() {
        std::vector<std::int16_t> resultSamples;
        while (true) {
            ComPtr<IMFSpatialAudioSample> spatialSample;
            ThrowIfFailed(CreateSpatialAudioSample(&spatialSample),
                          "Create live DTS:X spatial sample");
            ThrowIfFailed(AddSpatialAudioObjectBuffers(
                              spatialSample.Get(), metadataClient_.Get(), objectCount_, 512,
                              maxMetadataItems_),
                          "Allocate live DTS:X spatial buffers");
            ComPtr<IMFSample> sample;
            ThrowIfFailed(spatialSample.As(&sample), "Query live DTS:X output sample");

            MFT_OUTPUT_DATA_BUFFER output{};
            output.dwStreamID = 0;
            output.pSample = sample.Get();
            DWORD status = 0;
            const HRESULT result = decoder_.transform->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents != nullptr) output.pEvents->Release();
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
                throw std::runtime_error("Live DTS:X decoder changed output format");
            }
            ThrowIfFailed(result, "Decode live DTS:X output");
            auto interleaved = ConvertSample(spatialSample.Get());
            resultSamples.insert(resultSamples.end(), interleaved.begin(), interleaved.end());
            ++stats_.outputSamples;
        }
        return resultSamples;
    }

    std::vector<std::int16_t> ConvertSample(IMFSpatialAudioSample* sample) {
        DWORD objectCount = 0;
        ThrowIfFailed(sample->GetObjectCount(&objectCount),
                      "Get live DTS:X spatial object count");
        struct ObjectView {
            ComPtr<IMFSpatialAudioObjectBuffer> buffer;
            std::optional<std::size_t> speaker;
            DWORD bytes{};
        };
        std::vector<ObjectView> objects;
        objects.reserve(objectCount);
        std::size_t frameCount = 0;
        for (DWORD index = 0; index < objectCount; ++index) {
            ObjectView view;
            ThrowIfFailed(sample->GetSpatialAudioObjectByIndex(index, &view.buffer),
                          "Get live DTS:X spatial object");
            AudioObjectType type = AudioObjectType_None;
            UINT32 id = 0xffff'ffff;
            view.buffer->GetID(&id);
            view.buffer->GetType(&type);
            view.buffer->GetCurrentLength(&view.bytes);
            if (id != 0xffff'ffff && type != AudioObjectType_None) {
                view.speaker = layout_.FindSpeaker(AudioObjectTypeName(type));
                if (!view.speaker.has_value()) ++stats_.unmappedObjects;
            }
            if (view.bytes % sizeof(float) != 0) {
                throw std::runtime_error("DTS:X object has an incomplete float sample");
            }
            frameCount = std::max(frameCount,
                                  static_cast<std::size_t>(view.bytes / sizeof(float)));
            objects.push_back(std::move(view));
        }

        std::vector<double> mixed(frameCount * layout_.speakers.size());
        for (ObjectView& object : objects) {
            if (!object.speaker.has_value() || object.bytes == 0) continue;
            BYTE* raw = nullptr;
            DWORD maximum = 0;
            DWORD current = 0;
            ThrowIfFailed(object.buffer->Lock(&raw, &maximum, &current),
                          "Lock live DTS:X spatial object");
            const auto* values = reinterpret_cast<const float*>(raw);
            const std::size_t valuesCount = current / sizeof(float);
            for (std::size_t frame = 0; frame < valuesCount; ++frame) {
                const double value = std::isfinite(values[frame]) ? values[frame] : 0.0;
                mixed[frame * layout_.speakers.size() + *object.speaker] += value;
            }
            object.buffer->Unlock();
        }

        std::vector<std::int16_t> interleaved(mixed.size());
        for (std::size_t index = 0; index < mixed.size(); ++index) {
            if (mixed[index] < -1.0 || mixed[index] > 1.0) ++stats_.clippedSamples;
            const double value = std::clamp(mixed[index], -1.0, 1.0);
            interleaved[index] = static_cast<std::int16_t>(std::lround(value * 32'767.0));
        }
        stats_.pcmFrames += frameCount;
        return interleaved;
    }

    const SpeakerLayout& layout_;
    ActivatedDecoder decoder_;
    ComPtr<ISpatialAudioMetadataClient> metadataClient_;
    ComPtr<IMFMediaType> outputType_;
    MFT_OUTPUT_STREAM_INFO outputInfo_{};
    UINT32 objectCount_{};
    UINT32 maxMetadataItems_{};
    LONGLONG sampleTime_{};
    DtsXSpatialDecodeStats stats_;
    bool discontinuity_{true};
    bool streaming_{};
};

} // namespace

void ListAudioDecoders(const std::wstring& filter, const bool inspectTypes) {
    MediaFoundationSession mediaFoundation;
    IMFActivate** rawActivations = nullptr;
    UINT32 count = 0;
    ThrowIfFailed(MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_ALL,
                            nullptr, nullptr, &rawActivations, &count),
                  "Enumerate Media Foundation audio decoders");

    std::vector<ComPtr<IMFActivate>> activations;
    activations.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
        activations.emplace_back(rawActivations[index]);
    }
    CoTaskMemFree(rawActivations);

    UINT32 matches = 0;
    for (const auto& activation : activations) {
        const std::wstring name = ReadAllocatedString(activation.Get(), MFT_FRIENDLY_NAME_Attribute);
        GUID classId{};
        const HRESULT classResult = activation->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &classId);
        const std::wstring classText = SUCCEEDED(classResult) ? GuidText(classId) : L"(no CLSID)";
        if (!ContainsInsensitive(name + L" " + classText, filter)) continue;

        ++matches;
        std::wcout << L"\n" << (name.empty() ? L"(unnamed audio decoder)" : name) << L"\n"
                   << L"    CLSID: " << classText << L"\n";
        if (inspectTypes) {
            PrintAttributes(activation.Get());
            static constexpr GUID kPackagedCodecPath = {
                0x7347c815, 0x79fc, 0x4ad9,
                {0x87, 0x7d, 0xac, 0xdf, 0x5f, 0x46, 0x68, 0x5e}};
            const auto packagedCodecPath = ReadAllocatedString(activation.Get(), kPackagedCodecPath);
            LoadedModules loadedModules;
            ComPtr<IMFTransform> transform;
            const HRESULT result = ActivateDecoderObject(
                activation.Get(), packagedCodecPath, loadedModules, &transform);
            if (FAILED(result)) {
                std::wcout << L"    Activation failed: " << HResultText(result) << L"\n";
            } else {
                PrintAvailableTypes(transform.Get());
                activation->ShutdownObject();
            }
        }
    }
    std::wcout << L"\nMatched " << matches << L" of " << count
               << L" Media Foundation audio decoders.\n";
}

void ProbeWinRtDecoder(const std::wstring& runtimeClass) {
    MediaFoundationSession mediaFoundation;
    HString className(runtimeClass);
    ComPtr<IInspectable> instance;
    const HRESULT activation = RoActivateInstance(className.Get(), &instance);
    if (FAILED(activation)) {
        std::wcout << L"RoActivateInstance(" << runtimeClass << L") failed: "
                   << HResultText(activation) << L"\n";
        return;
    }

    PrintInspectableClass(instance.Get());
    ComPtr<IMFTransform> transform;
    const HRESULT query = instance.As(&transform);
    if (FAILED(query)) {
        std::wcout << L"IMFTransform is not exposed directly: " << HResultText(query) << L"\n";
        return;
    }
    std::wcout << L"IMFTransform is available.\n";
    PrintAvailableTypes(transform.Get());
}

void ProbeSpatialMetadata(const std::wstring& endpointFilter) {
    const Endpoint endpoint = SelectEndpoint(endpointFilter);
    ComPtr<ISpatialAudioClient> client;
    ThrowIfFailed(endpoint.device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL,
                                            nullptr, &client),
                  "Activate spatial audio client");

    AudioObjectType nativeMask = AudioObjectType_None;
    UINT32 maxDynamicObjects = 0;
    ThrowIfFailed(client->GetNativeStaticObjectTypeMask(&nativeMask),
                  "Get native spatial object mask");
    ThrowIfFailed(client->GetMaxDynamicObjectCount(&maxDynamicObjects),
                  "Get maximum dynamic object count");
    std::wcout << L"Endpoint: " << endpoint.name << L"\n"
               << L"Native static object mask: 0x" << std::hex
               << static_cast<std::uint32_t>(nativeMask) << std::dec << L"\n"
               << L"Maximum dynamic objects: " << maxDynamicObjects << L"\n";

    constexpr std::array<GUID, 3> metadataFormats = {{
        {0x9ddad816, 0xfcff, 0x4ae4,
         {0x8b, 0x6e, 0xad, 0x32, 0x4b, 0xd6, 0xd6, 0x9d}},
        {0x4e3aa922, 0x7258, 0x4575,
         {0xb6, 0x38, 0x80, 0xba, 0x88, 0x8f, 0x40, 0x90}},
        {0x2736caba, 0x57ce, 0x43dc,
         {0x9b, 0x5d, 0xfb, 0x14, 0xbf, 0x79, 0x07, 0xac}},
    }};
    std::wcout << L"Spatial metadata formats exposed by DTSXDecoder:\n";
    for (const GUID& format : metadataFormats) {
        PROPVARIANT auxiliary;
        PropVariantInit(&auxiliary);
        auxiliary.vt = VT_CLSID;
        auxiliary.puuid = const_cast<GUID*>(&format);
        const HRESULT result = client->IsSpatialAudioStreamAvailable(
            __uuidof(ISpatialAudioObjectRenderStreamForMetadata), &auxiliary);
        std::wcout << L"  " << GuidText(format) << L": "
                   << (SUCCEEDED(result) ? L"available" : HResultText(result)) << L"\n";
    }
}

void ProbeDtsXLicense(const std::wstring& codecName) {
    MediaFoundationSession mediaFoundation;
    using namespace winrt::Windows::ApplicationModel::AppService;
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Foundation::Collections;

    try {
        AppServiceConnection connection;
        connection.AppServiceName(L"com.DTSInc.DTSSoundUnbound.t5j2fzb");
        connection.PackageFamilyName(L"DTSInc.DTSSoundUnbound_t5j2fzbtdg37r");
        const auto openStatus = connection.OpenAsync().get();
        std::wcout << L"DTS license AppService open status: "
                   << static_cast<std::int32_t>(openStatus) << L"\n";
        if (openStatus != AppServiceConnectionStatus::Success) return;

        ValueSet request;
        request.Insert(L"Command", winrt::box_value(L"GetDecoderLicenseInfo"));
        request.Insert(L"MediaCodecName", winrt::box_value(codecName));
        const auto response = connection.SendMessageAsync(request).get();
        std::wcout << L"DTS license response status: "
                   << static_cast<std::int32_t>(response.Status()) << L"\n";
        for (const auto& entry : response.Message()) {
            std::wcout << L"  " << entry.Key().c_str() << L" = ";
            const auto property = entry.Value().try_as<IPropertyValue>();
            if (!property) {
                std::wcout << L"(non-property value)\n";
                continue;
            }
            switch (property.Type()) {
            case PropertyType::Boolean:
                std::wcout << (property.GetBoolean() ? L"true" : L"false");
                break;
            case PropertyType::String:
                std::wcout << property.GetString().c_str();
                break;
            case PropertyType::Int32:
                std::wcout << property.GetInt32();
                break;
            case PropertyType::UInt32:
                std::wcout << property.GetUInt32();
                break;
            default:
                std::wcout << L"property type " << static_cast<std::int32_t>(property.Type());
                break;
            }
            std::wcout << L"\n";
        }
    } catch (const winrt::hresult_error& error) {
        std::wcout << L"DTS license AppService error: " << HResultText(error.code())
                   << L" (" << error.message().c_str() << L")\n";
    }
}

void ProbeDtsXFieldOfUse() {
    MediaFoundationSession mediaFoundation;
    constexpr const wchar_t* decoderName = L"DTSXDecoder";
    struct EnumerationCase {
        const wchar_t* name;
        UINT32 flags;
    };
    constexpr std::array<EnumerationCase, 3> cases = {{
        {L"SYNCMFT", MFT_ENUM_FLAG_SYNCMFT},
        {L"SYNCMFT | FIELDOFUSE", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_FIELDOFUSE},
        {L"ALL", MFT_ENUM_FLAG_ALL},
    }};

    bool presentWithoutFieldOfUse = false;
    ComPtr<IMFActivate> activation;
    std::wcout << L"DTS:X Media Foundation registration:\n";
    for (const auto& enumeration : cases) {
        const auto activations = EnumerateAudioDecoders(enumeration.flags);
        const auto match = FindAudioDecoder(activations, decoderName);
        std::wcout << L"  " << enumeration.name << L": "
                   << (match ? L"present" : L"absent") << L" ("
                   << activations.size() << L" audio decoders)\n";
        if (enumeration.flags == MFT_ENUM_FLAG_SYNCMFT) {
            presentWithoutFieldOfUse = match != nullptr;
        }
        if (enumeration.flags == MFT_ENUM_FLAG_ALL) activation = match;
    }
    if (!activation) {
        std::wcout << L"DTSXDecoder was not returned by MFTEnumEx.\n";
        return;
    }

    std::wcout << L"Registration classification: "
               << (presentWithoutFieldOfUse
                       ? L"not field-of-use restricted (visible without the flag)"
                       : L"possibly field-of-use restricted")
               << L"\n";

    auto* rawUnlock = new FieldOfUseUnlockProbe();
    ComPtr<IMFFieldOfUseMFTUnlock> unlock;
    unlock.Attach(rawUnlock);
    ThrowIfFailed(activation->SetUnknown(MFT_FIELDOFUSE_UNLOCK_Attribute, unlock.Get()),
                  "Attach field-of-use unlock probe");

    static constexpr GUID kPackagedCodecPath = {
        0x7347c815, 0x79fc, 0x4ad9,
        {0x87, 0x7d, 0xac, 0xdf, 0x5f, 0x46, 0x68, 0x5e}};
    const auto codecPath = ReadAllocatedString(activation.Get(), kPackagedCodecPath);
    LoadedModules modules;
    ComPtr<IMFTransform> transform;
    const HRESULT activationResult = ActivateDecoderObject(
        activation.Get(), codecPath, modules, &transform);
    std::wcout << L"Activation with FOU callback: " << HResultText(activationResult) << L"\n"
               << L"Unlock callback calls: " << rawUnlock->Calls() << L"\n";
    if (SUCCEEDED(activationResult)) activation->ShutdownObject();
}

void ProbeDtsXDecode(const std::filesystem::path& inputPath, const std::size_t maxBursts,
                     const DtsXDecodeOutput outputMode,
                     const std::filesystem::path& outputPath) {
    MediaFoundationSession mediaFoundation;
    const auto frames = LoadDtsXFrames(inputPath, maxBursts);
    std::size_t totalFrameBytes = 0;
    for (const auto& frame : frames) totalFrameBytes += frame.size();
    const UINT32 measuredAverageBytes = static_cast<UINT32>(
        (totalFrameBytes * 48'000ULL) / (frames.size() * 512ULL));
    const GUID* requestedOutput = outputMode == DtsXDecodeOutput::SpatialObjects
        ? &MFAudioFormat_Float_SpatialObjects
        : &MFAudioFormat_PCM;
    auto decoder = ActivateAudioDecoder(L"DTSXDecoder", &kDtsXRawSubtype, requestedOutput);

    const Endpoint endpoint = SelectEndpoint(L"SinkDescription Sample");
    ComPtr<ISpatialAudioMetadataClient> metadataClient;
    if (outputMode == DtsXDecodeOutput::SpatialObjects) {
        PROPVARIANT metadataActivation;
        PropVariantInit(&metadataActivation);
        metadataActivation.vt = VT_CLSID;
        metadataActivation.puuid = const_cast<GUID*>(&kDtsXEndpointMetadataFormat);
        ThrowIfFailed(endpoint.device->Activate(__uuidof(ISpatialAudioMetadataClient), CLSCTX_ALL,
                                                &metadataActivation, &metadataClient),
                      "Activate spatial audio metadata client");
    }

    ComPtr<IMFAttributes> transformAttributes;
    if (SUCCEEDED(decoder.transform->GetAttributes(&transformAttributes))) {
        ThrowIfFailed(transformAttributes->SetString(MFT_AUDIO_DECODER_AUDIO_ENDPOINT_ID,
                                                      endpoint.id.c_str()),
                      "Set DTS:X decoder endpoint ID");
        if (metadataClient) {
            ThrowIfFailed(transformAttributes->SetUnknown(
                              MFT_AUDIO_DECODER_SPATIAL_METADATA_CLIENT, metadataClient.Get()),
                          "Set DTS:X spatial metadata client");
        }
        std::wcout << L"DTS:X transform attributes:\n";
        PrintAttributes(transformAttributes.Get());
    }
    ComPtr<IMFAttributes> inputAttributes;
    if (SUCCEEDED(decoder.transform->GetInputStreamAttributes(0, &inputAttributes))) {
        std::wcout << L"DTS:X input stream attributes:\n";
        PrintAttributes(inputAttributes.Get());
    }
    ComPtr<IMFAttributes> outputAttributes;
    if (SUCCEEDED(decoder.transform->GetOutputStreamAttributes(0, &outputAttributes))) {
        std::wcout << L"DTS:X output stream attributes:\n";
        PrintAttributes(outputAttributes.Get());
    }

    ComPtr<IMFMediaType> inputType;
    ThrowIfFailed(decoder.transform->GetInputAvailableType(0, 0, &inputType),
                  "Get DTS:X decoder input type");
    const ComPtr<IMFMediaType> advertisedInputType = inputType;
    std::wcout << L"DTS:X advertised input media type:\n";
    PrintAttributes(inputType.Get());

    ComPtr<IMFMediaType> outputType;
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> candidate;
        const HRESULT result = decoder.transform->GetOutputAvailableType(0, index, &candidate);
        if (result == MF_E_NO_MORE_TYPES) break;
        ThrowIfFailed(result, "Enumerate DTS:X decoder output types");
        GUID subtype{};
        if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
        if (outputMode == DtsXDecodeOutput::SpatialObjects) {
            GUID metadataFormat{};
            if (IsEqualGUID(subtype, MFAudioFormat_Float_SpatialObjects) &&
                SUCCEEDED(candidate->GetGUID(MF_MT_SPATIAL_AUDIO_OBJECT_METADATA_FORMAT_ID,
                                             &metadataFormat)) &&
                IsEqualGUID(metadataFormat, kDtsXEndpointMetadataFormat)) {
                outputType = candidate;
                break;
            }
        } else {
            UINT32 channels = 0;
            if ((IsEqualGUID(subtype, MFAudioFormat_PCM) ||
                 IsEqualGUID(subtype, MFAudioFormat_Float)) &&
                SUCCEEDED(candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) &&
                channels == 8) {
                outputType = candidate;
                break;
            }
        }
    }
    if (!outputType) {
        throw std::runtime_error(outputMode == DtsXDecodeOutput::SpatialObjects
                                     ? "DTS:X decoder did not expose the endpoint metadata format"
                                     : "DTS:X decoder did not expose 7.1 PCM output");
    }
    ThrowIfFailed(decoder.transform->SetOutputType(0, outputType.Get(), 0),
                  outputMode == DtsXDecodeOutput::SpatialObjects
                      ? "Set DTS:X spatial object output type"
                      : "Set DTS:X 7.1 PCM output type");
    inputType = ConfigureDtsXInputType(decoder.transform.Get(), advertisedInputType.Get(),
                                       measuredAverageBytes);
    if (!inputType) {
        throw std::runtime_error("No tested DTS:X input media profile was accepted");
    }
    ThrowIfFailed(decoder.transform->SetInputType(0, inputType.Get(), 0),
                  "Set DTS:X decoder input type");

    MFT_OUTPUT_STREAM_INFO outputInfo{};
    ThrowIfFailed(decoder.transform->GetOutputStreamInfo(0, &outputInfo),
                  "Get DTS:X output stream information");
    std::wcout << L"Input frames: " << frames.size() << L"\n"
               << L"Input type: " << MediaTypeText(inputType.Get()) << L"\n"
               << L"Output type: " << MediaTypeText(outputType.Get()) << L"\n"
               << L"Output stream: flags=0x" << std::hex << outputInfo.dwFlags << std::dec
               << L", cbSize=" << outputInfo.cbSize
               << L", alignment=" << outputInfo.cbAlignment << L"\n";

    std::unique_ptr<WaveWriter> pcmWriter;
    if (outputMode == DtsXDecodeOutput::Pcm71 && !outputPath.empty()) {
        WAVEFORMATEX* waveFormat = nullptr;
        UINT32 waveFormatBytes = 0;
        ThrowIfFailed(MFCreateWaveFormatExFromMFMediaType(
                          outputType.Get(), &waveFormat, &waveFormatBytes,
                          MFWaveFormatExConvertFlag_Normal),
                      "Create DTS:X PCM WAV format");
        try {
            pcmWriter = std::make_unique<WaveWriter>(outputPath, waveFormat);
        } catch (...) {
            CoTaskMemFree(waveFormat);
            throw;
        }
        CoTaskMemFree(waveFormat);
        std::wcout << L"PCM output WAV: " << outputPath.wstring() << L"\n";
    }

    const bool callerProvidesSamples =
        (outputInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                               MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0;
    UINT32 spatialObjectCount = 0;
    UINT32 maxMetadataItems = 0;
    if (outputMode == DtsXDecodeOutput::SpatialObjects) {
        ThrowIfFailed(outputType->GetUINT32(MF_MT_SPATIAL_AUDIO_MAX_DYNAMIC_OBJECTS,
                                            &spatialObjectCount),
                      "Get DTS:X maximum spatial object count");
        ThrowIfFailed(outputType->GetUINT32(MF_MT_SPATIAL_AUDIO_MAX_METADATA_ITEMS,
                                            &maxMetadataItems),
                      "Get DTS:X maximum metadata item count");
    }

    decoder.transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    ThrowIfFailed(decoder.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
                  "Begin DTS:X decoder streaming");
    ThrowIfFailed(decoder.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
                  "Start DTS:X decoder stream");

    std::size_t outputSamples = 0;
    const auto pullOutput = [&]() {
        while (true) {
            ComPtr<IMFSample> callerSample;
            if (callerProvidesSamples) {
                if (outputMode == DtsXDecodeOutput::SpatialObjects) {
                    ComPtr<IMFSpatialAudioSample> spatialSample;
                    ThrowIfFailed(CreateSpatialAudioSample(&spatialSample),
                                  "Create DTS:X spatial output sample");
                    ThrowIfFailed(spatialSample.As(&callerSample),
                                  "Query DTS:X spatial sample as IMFSample");
                    ThrowIfFailed(AddSpatialAudioObjectBuffers(
                                      spatialSample.Get(), metadataClient.Get(),
                                      spatialObjectCount, 512, maxMetadataItems),
                                  "Allocate DTS:X spatial object buffers");
                } else {
                    ThrowIfFailed(MFCreateSample(&callerSample),
                                  "Create DTS:X PCM output sample");
                    if (outputInfo.cbSize != 0) {
                        ComPtr<IMFMediaBuffer> outputBuffer;
                        ThrowIfFailed(MFCreateMemoryBuffer(outputInfo.cbSize, &outputBuffer),
                                      "Create DTS:X output buffer");
                        ThrowIfFailed(callerSample->AddBuffer(outputBuffer.Get()),
                                      "Attach DTS:X output buffer");
                    }
                }
            }

            MFT_OUTPUT_DATA_BUFFER output{};
            output.dwStreamID = 0;
            output.pSample = callerSample.Get();
            DWORD status = 0;
            const HRESULT result = decoder.transform->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents != nullptr) output.pEvents->Release();
            ComPtr<IMFSample> sample;
            if (callerSample) {
                sample = callerSample;
            } else if (output.pSample != nullptr) {
                sample.Attach(output.pSample);
            }
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
                throw std::runtime_error("DTS:X decoder changed its output stream format");
            }
            ThrowIfFailed(result, "Decode DTS:X output");
            if (!sample) throw std::runtime_error("DTS:X decoder returned an empty output sample");
            if (outputMode == DtsXDecodeOutput::SpatialObjects) {
                if (outputSamples < 4) PrintSpatialSample(sample.Get(), outputSamples);
            } else {
                PrintPcmSample(sample.Get(), outputType.Get(), outputSamples, pcmWriter.get());
            }
            ++outputSamples;
        }
    };

    constexpr LONGLONG frameDuration = (10'000'000LL * 512) / 48'000;
    LONGLONG sampleTime = 0;
    for (const auto& frame : frames) {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        ThrowIfFailed(MFCreateSample(&sample), "Create DTS:X input sample");
        ThrowIfFailed(MFCreateMemoryBuffer(static_cast<DWORD>(frame.size()), &buffer),
                      "Create DTS:X input buffer");
        BYTE* destination = nullptr;
        DWORD maximum = 0;
        ThrowIfFailed(buffer->Lock(&destination, &maximum, nullptr), "Lock DTS:X input buffer");
        std::memcpy(destination, frame.data(), frame.size());
        buffer->Unlock();
        ThrowIfFailed(buffer->SetCurrentLength(static_cast<DWORD>(frame.size())),
                      "Set DTS:X input length");
        ThrowIfFailed(sample->AddBuffer(buffer.Get()), "Attach DTS:X input buffer");
        ThrowIfFailed(sample->SetSampleTime(sampleTime), "Set DTS:X sample time");
        ThrowIfFailed(sample->SetSampleDuration(frameDuration), "Set DTS:X sample duration");
        sampleTime += frameDuration;

        HRESULT result = decoder.transform->ProcessInput(0, sample.Get(), 0);
        if (result == MF_E_NOTACCEPTING) {
            pullOutput();
            result = decoder.transform->ProcessInput(0, sample.Get(), 0);
        }
        ThrowIfFailed(result, "Submit DTS:X input frame");
        pullOutput();
    }
    ThrowIfFailed(decoder.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0),
                  "End DTS:X decoder stream");
    ThrowIfFailed(decoder.transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0),
                  "Drain DTS:X decoder");
    pullOutput();
    decoder.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    if (pcmWriter) pcmWriter->Finalize();
    std::wcout << (outputMode == DtsXDecodeOutput::SpatialObjects
                       ? L"Decoded spatial samples: "
                       : L"Decoded PCM samples: ")
               << outputSamples << L"\n";
}

void PlayLiveDtsXLayout(const double seconds,
                        const std::filesystem::path& layoutPath,
                        const double gain,
                        const DWORD prebufferMilliseconds,
                        const RendererLatencyMode latencyMode) {
    MediaFoundationSession mediaFoundation;
    const SpeakerLayout layout = LoadSpeakerLayout(layoutPath);
    WinHandle device = OpenMatCaptureDevice();
    BridgeMeterPublisher meters(layout, BridgeMeterMode::DtsX);
    MultiEndpointRenderer renderer(layout, gain, latencyMode);
    DtsXSpatialDecoder decoder(layout);
    ResetMatCapture(device.Get());
    DtsXCarrierFramer framer;
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
    bool decoderDrained = false;

    std::wcout << L"Live DTS:X configurable layout: " << layout.name << L"\n"
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
    std::wcout << L"  decoder=DTSXDecoder spatial 7.1.4, gain=" << std::fixed
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
                    if (!IsEqualGUID(read.header.SubFormat, kDtsXE1)) {
                        throw std::runtime_error(
                            "Live DTS:X requires the SinkDescription DTS:X E1 format");
                    }
                    if (read.header.FormatChanges != 0) {
                        throw std::runtime_error("IEC 61937 format changed during DTS:X playback");
                    }
                    if (haveSequence && read.header.FirstByteSequence != expectedSequence) {
                        sequenceGaps += read.header.FirstByteSequence > expectedSequence
                            ? read.header.FirstByteSequence - expectedSequence
                            : expectedSequence - read.header.FirstByteSequence;
                        framer.Reset();
                        decoder.Reset();
                    }
                    haveSequence = true;
                    expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
                    ringBytes += read.header.PayloadBytes;
                    lastPayloadTime = now;

                    auto frames = framer.Push(read.payload, read.header.PayloadBytes);
                    for (const auto& frame : frames) {
                        auto pcm = decoder.Push(frame);
                        if (!pcm.empty()) {
                            meters.Update(pcm);
                            queue.Append(std::move(pcm));
                        }
                    }
                    maximumQueuedFrames = std::max(
                        maximumQueuedFrames, renderer.MinimumFramesAvailable(queue));
                }
                if (!unlimited && now >= deadline) acceptingInput = false;
            }

            if (!acceptingInput && !decoderDrained) {
                auto pcm = decoder.Drain();
                if (!pcm.empty()) {
                    meters.Update(pcm);
                    queue.Append(std::move(pcm));
                }
                decoderDrained = true;
            }

            const std::uint64_t queuedForAll = renderer.MinimumFramesAvailable(queue);
            if (!renderer.IsStarted() &&
                (queuedForAll >= prebufferFrames || (decoderDrained && queuedForAll != 0))) {
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

            if (decoderDrained && (!renderer.IsStarted() || renderer.IsDrained(queue))) break;
        }
    } catch (...) {
        renderer.Stop();
        throw;
    }

    if (!renderer.IsStarted()) {
        throw std::runtime_error("No complete DTS:X E1 bursts arrived before the live timeout");
    }
    renderer.Stop();
    const MAT_CAPTURE_STATS ringStats = QueryMatCaptureStats(device.Get());
    const DtsXSpatialDecodeStats& decodeStats = decoder.Stats();
    std::wcout << L"Live DTS:X layout playback complete\n"
               << L"  ring bytes=" << ringBytes << L", driver dropped="
               << ringStats.DroppedBytes << L", sequence gaps=" << sequenceGaps << L"\n"
               << L"  bursts=" << framer.Bursts() << L", malformed="
               << framer.MalformedBursts() << L", carrier skipped="
               << framer.SkippedBytes() << L", buffered=" << framer.BufferedBytes() << L"\n"
               << L"  decoder input=" << decodeStats.inputFrames << L", output="
               << decodeStats.outputSamples << L", PCM frames=" << decodeStats.pcmFrames
               << L", clipped=" << decodeStats.clippedSamples << L", unmapped objects="
               << decodeStats.unmappedObjects << L"\n"
               << L"  max PCM queue=" << maximumQueuedFrames << L" frames\n";
    PrintEndpointRenderStats(renderer.Stats());
}

void ProbeMediaTypes(const std::filesystem::path& inputPath) {
    MediaFoundationSession mediaFoundation;
    ComPtr<IMFSourceReader> reader;
    ThrowIfFailed(MFCreateSourceReaderFromURL(inputPath.c_str(), nullptr, &reader),
                  "Open media source");

    std::wcout << L"Media source: " << inputPath.wstring() << L"\n";
    bool testedDtsDecoder = false;
    for (DWORD streamIndex = 0;; ++streamIndex) {
        bool foundType = false;
        for (DWORD typeIndex = 0;; ++typeIndex) {
            ComPtr<IMFMediaType> type;
            const HRESULT result = reader->GetNativeMediaType(streamIndex, typeIndex, &type);
            if (result == MF_E_INVALIDSTREAMNUMBER) {
                if (typeIndex == 0) return;
                break;
            }
            if (result == MF_E_NO_MORE_TYPES) break;
            ThrowIfFailed(result, "Get native media source type");
            foundType = true;
            std::wcout << L"Stream " << streamIndex << L", native type " << typeIndex
                       << L": " << MediaTypeText(type.Get()) << L"\n";
            PrintAttributes(type.Get());
            GUID major{};
            GUID subtype{};
            if (!testedDtsDecoder &&
                SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
                IsEqualGUID(major, MFMediaType_Audio) &&
                SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                IsEqualGUID(subtype, MFAudioFormat_DTS_HD)) {
                testedDtsDecoder = true;
                ComPtr<IMFMediaType> requestedPcm;
                ThrowIfFailed(MFCreateMediaType(&requestedPcm),
                              "Create SourceReader PCM request");
                requestedPcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                requestedPcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                const HRESULT sourceReaderResult = reader->SetCurrentMediaType(
                    streamIndex, nullptr, requestedPcm.Get());
                std::wcout << L"SourceReader DTS-HD to PCM negotiation: "
                           << HResultText(sourceReaderResult) << L"\n";
                if (SUCCEEDED(sourceReaderResult)) {
                    ComPtr<IMFMediaType> currentType;
                    ThrowIfFailed(reader->GetCurrentMediaType(streamIndex, &currentType),
                                  "Get SourceReader decoded PCM type");
                    std::wcout << L"SourceReader output: "
                               << MediaTypeText(currentType.Get()) << L"\n";
                    DWORD actualStream = 0;
                    DWORD flags = 0;
                    LONGLONG timestamp = 0;
                    ComPtr<IMFSample> decodedSample;
                    const HRESULT readResult = reader->ReadSample(
                        streamIndex, 0, &actualStream, &flags, &timestamp, &decodedSample);
                    DWORD decodedBytes = 0;
                    if (decodedSample) decodedSample->GetTotalLength(&decodedBytes);
                    std::wcout << L"SourceReader first decoded sample: "
                               << HResultText(readResult) << L", stream=" << actualStream
                               << L", flags=0x" << std::hex << flags << std::dec
                               << L", time=" << timestamp << L", bytes=" << decodedBytes
                               << L"\n";
                }
                auto decoder = ActivateAudioDecoder(
                    L"DTSXDecoder", &MFAudioFormat_DTS_HD, &MFAudioFormat_PCM);
                ComPtr<IMFMediaType> pcmType;
                for (DWORD outputIndex = 0;; ++outputIndex) {
                    ComPtr<IMFMediaType> candidate;
                    const HRESULT outputResult = decoder.transform->GetOutputAvailableType(
                        0, outputIndex, &candidate);
                    if (outputResult == MF_E_NO_MORE_TYPES) break;
                    ThrowIfFailed(outputResult, "Enumerate DTS-HD comparison output");
                    GUID outputSubtype{};
                    if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &outputSubtype)) &&
                        IsEqualGUID(outputSubtype, MFAudioFormat_PCM)) {
                        pcmType = candidate;
                        break;
                    }
                }
                const HRESULT outputResult = pcmType
                    ? decoder.transform->SetOutputType(0, pcmType.Get(), 0)
                    : MF_E_INVALIDMEDIATYPE;
                const HRESULT inputResult = decoder.transform->SetInputType(
                    0, type.Get(), MFT_SET_TYPE_TEST_ONLY);
                auto inputFirstDecoder = ActivateAudioDecoder(
                    L"DTSXDecoder", &MFAudioFormat_DTS_HD, &MFAudioFormat_PCM);
                const HRESULT inputFirstResult = inputFirstDecoder.transform->SetInputType(
                    0, type.Get(), MFT_SET_TYPE_TEST_ONLY);
                std::wcout << L"DTS-HD decoder comparison: output="
                           << HResultText(outputResult) << L", exact native input="
                           << HResultText(inputResult) << L", input-first="
                           << HResultText(inputFirstResult) << L"\n";
            }
        }
        if (!foundType) std::wcout << L"Stream " << streamIndex << L": no native types\n";
    }
}

} // namespace dolby
