#include "audio_platform.h"
#include "commands.h"
#include "mat_format.h"
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
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dolby {
namespace {

inline constexpr GUID kDtsXEndpointMetadataFormat = {
    0x2736caba, 0x57ce, 0x43dc,
    {0x9b, 0x5d, 0xfb, 0x14, 0xbf, 0x79, 0x07, 0xac}};

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
        for (const UINT32 channels : {6U, 8U, 12U}) {
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

void ProbeDtsXDecode(const std::filesystem::path& inputPath, const std::size_t maxBursts) {
    MediaFoundationSession mediaFoundation;
    const auto frames = LoadDtsXFrames(inputPath, maxBursts);
    std::size_t totalFrameBytes = 0;
    for (const auto& frame : frames) totalFrameBytes += frame.size();
    const UINT32 measuredAverageBytes = static_cast<UINT32>(
        (totalFrameBytes * 48'000ULL) / (frames.size() * 512ULL));
    static constexpr GUID kDtsXRawSubtype = {
        0x64747378, 0x767a, 0x494d,
        {0xb4, 0x78, 0xf2, 0x9d, 0x25, 0xdc, 0x90, 0x37}};
    auto decoder = ActivateAudioDecoder(
        L"DTSXDecoder", &kDtsXRawSubtype, &MFAudioFormat_Float_SpatialObjects);

    const Endpoint endpoint = SelectEndpoint(L"SinkDescription Sample");
    PROPVARIANT metadataActivation;
    PropVariantInit(&metadataActivation);
    metadataActivation.vt = VT_CLSID;
    metadataActivation.puuid = const_cast<GUID*>(&kDtsXEndpointMetadataFormat);
    ComPtr<ISpatialAudioMetadataClient> metadataClient;
    ThrowIfFailed(endpoint.device->Activate(__uuidof(ISpatialAudioMetadataClient), CLSCTX_ALL,
                                            &metadataActivation, &metadataClient),
                  "Activate spatial audio metadata client");

    ComPtr<IMFAttributes> transformAttributes;
    if (SUCCEEDED(decoder.transform->GetAttributes(&transformAttributes))) {
        ThrowIfFailed(transformAttributes->SetString(MFT_AUDIO_DECODER_AUDIO_ENDPOINT_ID,
                                                      endpoint.id.c_str()),
                      "Set DTS:X decoder endpoint ID");
        ThrowIfFailed(transformAttributes->SetUnknown(MFT_AUDIO_DECODER_SPATIAL_METADATA_CLIENT,
                                                       metadataClient.Get()),
                      "Set DTS:X spatial metadata client");
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
        GUID metadataFormat{};
        if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            IsEqualGUID(subtype, MFAudioFormat_Float_SpatialObjects) &&
            SUCCEEDED(candidate->GetGUID(MF_MT_SPATIAL_AUDIO_OBJECT_METADATA_FORMAT_ID,
                                         &metadataFormat)) &&
            IsEqualGUID(metadataFormat, kDtsXEndpointMetadataFormat)) {
            outputType = candidate;
            break;
        }
    }
    if (!outputType) {
        throw std::runtime_error("DTS:X decoder did not expose the endpoint metadata format");
    }
    ThrowIfFailed(decoder.transform->SetOutputType(0, outputType.Get(), 0),
                  "Set DTS:X spatial object output type");
    inputType = ConfigureDtsXInputType(decoder.transform.Get(), advertisedInputType.Get(),
                                       measuredAverageBytes);
    bool spatialOutput = true;
    if (!inputType) {
        ComPtr<IMFMediaType> pcmOutput;
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> candidate;
            const HRESULT result = decoder.transform->GetOutputAvailableType(0, index, &candidate);
            if (result == MF_E_NO_MORE_TYPES) break;
            ThrowIfFailed(result, "Enumerate DTS:X PCM output types");
            GUID subtype{};
            UINT32 channels = 0;
            if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                IsEqualGUID(subtype, MFAudioFormat_PCM) &&
                SUCCEEDED(candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) &&
                channels == 8) {
                pcmOutput = candidate;
                break;
            }
        }
        if (pcmOutput) {
            ThrowIfFailed(decoder.transform->SetOutputType(0, pcmOutput.Get(), 0),
                          "Set DTS:X PCM comparison output type");
            inputType = ConfigureDtsXInputType(decoder.transform.Get(),
                                               advertisedInputType.Get(), measuredAverageBytes);
            if (inputType) {
                outputType = pcmOutput;
                spatialOutput = false;
            }
        }
    }
    if (!inputType) {
        throw std::runtime_error("No tested DTS:X input media profile was accepted");
    }
    ThrowIfFailed(decoder.transform->SetInputType(0, inputType.Get(), 0),
                  "Set DTS:X decoder input type");
    if (!spatialOutput) {
        std::wcout << L"The input was accepted only after selecting PCM 7.1 output.\n";
        throw std::runtime_error("DTS:X spatial-object negotiation is still incomplete");
    }

    MFT_OUTPUT_STREAM_INFO outputInfo{};
    ThrowIfFailed(decoder.transform->GetOutputStreamInfo(0, &outputInfo),
                  "Get DTS:X output stream information");
    std::wcout << L"Input frames: " << frames.size() << L"\n"
               << L"Input type: " << MediaTypeText(inputType.Get()) << L"\n"
               << L"Output type: " << MediaTypeText(outputType.Get()) << L"\n"
               << L"Output stream: flags=0x" << std::hex << outputInfo.dwFlags << std::dec
               << L", cbSize=" << outputInfo.cbSize
               << L", alignment=" << outputInfo.cbAlignment << L"\n";

    if ((outputInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                               MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0) {
        throw std::runtime_error(
            "DTS:X spatial output requires a caller-created IMFSpatialAudioSample");
    }

    decoder.transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    ThrowIfFailed(decoder.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
                  "Begin DTS:X decoder streaming");
    ThrowIfFailed(decoder.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
                  "Start DTS:X decoder stream");

    std::size_t outputSamples = 0;
    const auto pullOutput = [&]() {
        while (true) {
            MFT_OUTPUT_DATA_BUFFER output{};
            output.dwStreamID = 0;
            DWORD status = 0;
            const HRESULT result = decoder.transform->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents != nullptr) output.pEvents->Release();
            ComPtr<IMFSample> sample;
            if (output.pSample != nullptr) sample.Attach(output.pSample);
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
                throw std::runtime_error("DTS:X decoder changed its output stream format");
            }
            ThrowIfFailed(result, "Decode DTS:X output");
            if (!sample) throw std::runtime_error("DTS:X decoder returned an empty output sample");
            if (outputSamples < 4) PrintSpatialSample(sample.Get(), outputSamples);
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
    std::wcout << L"Decoded spatial samples: " << outputSamples << L"\n";
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
