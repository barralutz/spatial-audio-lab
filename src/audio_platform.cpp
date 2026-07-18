#define INITGUID
#include "audio_platform.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propkeydef.h>

#include <algorithm>
#include <cwctype>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <limits>
#include <string_view>

namespace dolby {

class __declspec(uuid("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9")) PolicyConfigClient;

MIDL_INTERFACE("F8679F50-850A-41CF-9C72-430F290290C8")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(LPCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(LPCWSTR, BOOL, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(LPCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(LPCWSTR, BOOL, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(LPCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(LPCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(LPCWSTR, const PROPERTYKEY&,
                                                       const PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(LPCWSTR, BOOL) = 0;
};

std::wstring HResultText(const HRESULT result) {
    std::wostringstream text;
    text << L"0x" << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0')
         << static_cast<std::uint32_t>(result);
    return text.str();
}

namespace {

std::string AudioClientErrorMessage(const HRESULT result, const char* operation) {
    std::ostringstream message;
    message << operation << " failed (0x" << std::hex
            << static_cast<std::uint32_t>(result) << ')';
    return message.str();
}

} // namespace

AudioClientError::AudioClientError(const HRESULT result, const char* operation)
    : std::runtime_error(AudioClientErrorMessage(result, operation)), result_(result) {}

void ThrowIfFailed(const HRESULT result, const char* operation) {
    if (FAILED(result)) throw AudioClientError(result, operation);
}

bool IsRecoverableAudioClientError(const HRESULT result) {
    return result == AUDCLNT_E_DEVICE_INVALIDATED ||
           result == AUDCLNT_E_RESOURCES_INVALIDATED ||
           result == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

std::wstring GuidText(const GUID& guid) {
    wchar_t buffer[64]{};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    return value;
}

double Decibels(const double linear) {
    return linear > 0.0 ? 20.0 * std::log10(linear)
                        : -std::numeric_limits<double>::infinity();
}

std::wstring FormFactorName(const UINT32 value) {
    switch (static_cast<EndpointFormFactor>(value)) {
    case RemoteNetworkDevice: return L"RemoteNetworkDevice";
    case Speakers: return L"Speakers";
    case LineLevel: return L"LineLevel";
    case Headphones: return L"Headphones";
    case Microphone: return L"Microphone";
    case Headset: return L"Headset";
    case Handset: return L"Handset";
    case UnknownDigitalPassthrough: return L"UnknownDigitalPassthrough";
    case SPDIF: return L"SPDIF";
    case DigitalAudioDisplayDevice: return L"DigitalAudioDisplayDevice";
    case UnknownFormFactor: return L"UnknownFormFactor";
    default: return L"Unknown(" + std::to_wstring(value) + L")";
    }
}

std::wstring SubformatName(const GUID& guid) {
    if (IsEqualGUID(guid, kPcm)) return L"PCM";
    if (IsEqualGUID(guid, kIeeeFloat)) return L"IEEE_FLOAT";
    if (IsEqualGUID(guid, kDolbyDigitalPlus)) return L"Dolby Digital Plus";
    if (IsEqualGUID(guid, kDolbyDigitalPlusAtmos)) return L"Dolby Atmos (DD+)";
    if (IsEqualGUID(guid, kDolbyMlpMat10)) return L"Dolby MLP / MAT 1.0";
    if (IsEqualGUID(guid, kDolbyMat20)) return L"Dolby MAT 2.0";
    if (IsEqualGUID(guid, kDolbyMat21Profile3)) return L"Dolby MAT 2.1 Profile 3";
    if (IsEqualGUID(guid, kDolbyMat21Profile4)) return L"Dolby MAT 2.1 Profile 4";
    if (IsEqualGUID(guid, kDts)) return L"DTS";
    if (IsEqualGUID(guid, kDtsHd)) return L"DTS-HD";
    if (IsEqualGUID(guid, kDtsXE1)) return L"DTS:X E1";
    if (IsEqualGUID(guid, kDtsXE2)) return L"DTS:X E2";
    return GuidText(guid);
}

std::wstring WaveFormatText(const WAVEFORMATEX* format) {
    std::wostringstream text;
    text << format->nChannels << L"ch, " << format->nSamplesPerSec << L" Hz, "
         << format->wBitsPerSample << L" bit, block=" << format->nBlockAlign;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        text << L", mask=0x" << std::hex << std::uppercase << extensible->dwChannelMask
             << L", " << SubformatName(extensible->SubFormat);
    } else {
        text << L", tag=0x" << std::hex << std::uppercase << format->wFormatTag;
    }
    return text.str();
}

std::wstring ReadStringProperty(IPropertyStore* properties, const PROPERTYKEY& key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT result = properties->GetValue(key, &value);
    std::wstring output;
    if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        output = value.pwszVal;
    }
    PropVariantClear(&value);
    return output;
}

bool ReadUintProperty(IPropertyStore* properties, const PROPERTYKEY& key, UINT32& output) {
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT result = properties->GetValue(key, &value);
    if (SUCCEEDED(result) && value.vt == VT_UI4) {
        output = value.ulVal;
        PropVariantClear(&value);
        return true;
    }
    PropVariantClear(&value);
    return false;
}

std::vector<Endpoint> EnumerateRenderEndpoints() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ThrowIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator)),
                  "Create MMDeviceEnumerator");

    ComPtr<IMMDevice> defaultDevice;
    LPWSTR defaultIdRaw = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
        defaultDevice->GetId(&defaultIdRaw);
    }
    const std::wstring defaultId = defaultIdRaw != nullptr ? defaultIdRaw : L"";
    CoTaskMemFree(defaultIdRaw);

    ComPtr<IMMDeviceCollection> collection;
    ThrowIfFailed(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection),
                  "Enumerate render endpoints");

    UINT count = 0;
    ThrowIfFailed(collection->GetCount(&count), "Get endpoint count");
    std::vector<Endpoint> endpoints;
    endpoints.reserve(count);

    for (UINT index = 0; index < count; ++index) {
        Endpoint endpoint;
        ThrowIfFailed(collection->Item(index, &endpoint.device), "Get endpoint");

        LPWSTR idRaw = nullptr;
        ThrowIfFailed(endpoint.device->GetId(&idRaw), "Get endpoint ID");
        endpoint.id = idRaw;
        CoTaskMemFree(idRaw);
        endpoint.isDefault = endpoint.id == defaultId;

        ComPtr<IPropertyStore> properties;
        ThrowIfFailed(endpoint.device->OpenPropertyStore(STGM_READ, &properties),
                      "Open endpoint properties");
        endpoint.name = ReadStringProperty(properties.Get(), PKEY_Device_FriendlyName);
        endpoints.push_back(std::move(endpoint));
    }
    return endpoints;
}

Endpoint SelectEndpoint(const std::wstring& filter) {
    auto endpoints = EnumerateRenderEndpoints();
    if (filter.empty()) {
        const auto found = std::find_if(endpoints.begin(), endpoints.end(),
                                        [](const Endpoint& endpoint) { return endpoint.isDefault; });
        if (found != endpoints.end()) return *found;
        throw std::runtime_error("No default render endpoint");
    }

    const std::wstring needle = Lowercase(filter);
    const auto found = std::find_if(endpoints.begin(), endpoints.end(), [&](const Endpoint& endpoint) {
        return Lowercase(endpoint.name).find(needle) != std::wstring::npos ||
               Lowercase(endpoint.id).find(needle) != std::wstring::npos;
    });
    if (found == endpoints.end()) {
        throw std::runtime_error("No endpoint matched the requested filter");
    }
    return *found;
}

void SetDefaultEndpoint(const std::wstring& filter) {
    const Endpoint endpoint = SelectEndpoint(filter);
    ComPtr<IPolicyConfig> policy;
    ThrowIfFailed(CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&policy)),
                  "Create audio policy client");
    for (const ERole role : {eConsole, eMultimedia, eCommunications}) {
        ThrowIfFailed(policy->SetDefaultEndpoint(endpoint.id.c_str(), role),
                      "Set default audio endpoint");
    }
    std::wcout << L"Default render endpoint (all roles): " << endpoint.name << L"\n"
               << L"ID: " << endpoint.id << L"\n";
}

void SetNativePcm714Format(const std::wstring& filter) {
    constexpr DWORD channelMask = 0x0002D63F;
    Endpoint endpoint = SelectEndpoint(filter);
    WAVEFORMATEXTENSIBLE pcm{};
    pcm.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    pcm.Format.nChannels = 12;
    pcm.Format.nSamplesPerSec = 48'000;
    pcm.Format.wBitsPerSample = 16;
    pcm.Format.nBlockAlign = 24;
    pcm.Format.nAvgBytesPerSec = 1'152'000;
    pcm.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    pcm.Samples.wValidBitsPerSample = 16;
    pcm.dwChannelMask = channelMask;
    pcm.SubFormat = kPcm;
    WAVEFORMATEXTENSIBLE mix{};
    mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix.Format.nSamplesPerSec = 48'000;
    mix.Format.wBitsPerSample = 32;
    mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    mix.Samples.wValidBitsPerSample = 32;
    mix.SubFormat = kIeeeFloat;
    mix.Format.nChannels = 12;
    mix.Format.nBlockAlign = 48;
    mix.Format.nAvgBytesPerSec = 2'304'000;
    mix.dwChannelMask = channelMask;

    ComPtr<IAudioClient> client;
    ThrowIfFailed(endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                             nullptr, &client),
                  "Activate PCM 7.1.4 endpoint");
    const HRESULT support = client->IsFormatSupported(
        AUDCLNT_SHAREMODE_EXCLUSIVE, &pcm.Format, nullptr);
    if (support != S_OK) {
        ThrowIfFailed(support, "Validate selected endpoint format");
        throw std::runtime_error("The endpoint did not accept the selected format exactly");
    }

    ComPtr<IPolicyConfig> policy;
    ThrowIfFailed(CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&policy)),
                  "Create audio policy client");
    ThrowIfFailed(policy->SetDeviceFormat(endpoint.id.c_str(),
                                           &pcm.Format, &mix.Format),
                  "Set native PCM 7.1.4 endpoint format");
    std::wcout << L"Native PCM 7.1.4 device and mix format selected: "
               << endpoint.name << L"\n"
               << L"ID: " << endpoint.id << L"\n";
}

void SetSpatialCodecFormat(const std::wstring& filter, const bool dtsX) {
    Endpoint endpoint = SelectEndpoint(filter);
    WAVEFORMATEXTENSIBLE carrier{};
    carrier.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    carrier.Format.nChannels = 8;
    carrier.Format.nSamplesPerSec = 192'000;
    carrier.Format.nAvgBytesPerSec = 3'072'000;
    carrier.Format.nBlockAlign = 16;
    carrier.Format.wBitsPerSample = 16;
    carrier.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    carrier.Samples.wValidBitsPerSample = 16;
    carrier.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
    carrier.SubFormat = dtsX ? kDtsXE1 : kDolbyMat21Profile3;
    WAVEFORMATEXTENSIBLE mix{};
    mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix.Format.nChannels = 8;
    mix.Format.nSamplesPerSec = 48'000;
    mix.Format.nAvgBytesPerSec = 1'536'000;
    mix.Format.nBlockAlign = 32;
    mix.Format.wBitsPerSample = 32;
    mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    mix.Samples.wValidBitsPerSample = 32;
    mix.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
    mix.SubFormat = kIeeeFloat;

    ComPtr<IAudioClient> client;
    ThrowIfFailed(endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                             nullptr, &client),
                  "Activate spatial codec endpoint");
    const HRESULT support = client->IsFormatSupported(
        AUDCLNT_SHAREMODE_EXCLUSIVE, &carrier.Format, nullptr);
    if (support != S_OK) {
        ThrowIfFailed(support, "Validate spatial codec endpoint format");
        throw std::runtime_error("The endpoint did not accept the spatial codec format exactly");
    }

    ComPtr<IPolicyConfig> policy;
    ThrowIfFailed(CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&policy)),
                  "Create audio policy client");
    ThrowIfFailed(policy->SetDeviceFormat(endpoint.id.c_str(),
                                           &carrier.Format, &mix.Format),
                  "Set spatial codec endpoint format");
    std::wcout << (dtsX ? L"DTS:X E1" : L"Dolby MAT 2.1 Profile 3")
               << L" device format and 7.1 mix selected: " << endpoint.name << L"\n"
               << L"ID: " << endpoint.id << L"\n";
}

void SetLegacyMatFormat(const std::wstring& filter) {
    Endpoint endpoint = SelectEndpoint(filter);
    WAVEFORMATEXTENSIBLE carrier{};
    carrier.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    carrier.Format.nChannels = 8;
    carrier.Format.nSamplesPerSec = 192'000;
    carrier.Format.nAvgBytesPerSec = 3'072'000;
    carrier.Format.nBlockAlign = 16;
    carrier.Format.wBitsPerSample = 16;
    carrier.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    carrier.Samples.wValidBitsPerSample = 16;
    carrier.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
    carrier.SubFormat = kDolbyMlpMat10;
    WAVEFORMATEXTENSIBLE mix{};
    mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix.Format.nChannels = 8;
    mix.Format.nSamplesPerSec = 48'000;
    mix.Format.nAvgBytesPerSec = 1'536'000;
    mix.Format.nBlockAlign = 32;
    mix.Format.wBitsPerSample = 32;
    mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    mix.Samples.wValidBitsPerSample = 32;
    mix.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
    mix.SubFormat = kIeeeFloat;

    ComPtr<IAudioClient> client;
    ThrowIfFailed(endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                             nullptr, &client),
                  "Activate legacy MAT endpoint");
    const HRESULT support = client->IsFormatSupported(
        AUDCLNT_SHAREMODE_EXCLUSIVE, &carrier.Format, nullptr);
    if (support != S_OK) {
        ThrowIfFailed(support, "Validate legacy MAT endpoint format");
        throw std::runtime_error("The endpoint did not accept Dolby MLP / MAT 1.0 exactly");
    }

    ComPtr<IPolicyConfig> policy;
    ThrowIfFailed(CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&policy)),
                  "Create audio policy client");
    ThrowIfFailed(policy->SetDeviceFormat(endpoint.id.c_str(),
                                           &carrier.Format, &mix.Format),
                  "Set legacy MAT endpoint format");
    std::wcout << L"Dolby MLP / MAT 1.0 device format and 7.1 mix selected: "
               << endpoint.name << L"\n"
               << L"ID: " << endpoint.id << L"\n";
}

namespace {

void PrintPolicyFormat(const wchar_t* label, const HRESULT result, WAVEFORMATEX* format) {
    std::wcout << L"  " << label << L": ";
    if (SUCCEEDED(result) && format != nullptr) {
        std::wcout << WaveFormatText(format) << L"\n";
    } else {
        std::wcout << HResultText(result) << L"\n";
    }
    CoTaskMemFree(format);
}

} // namespace

void PrintConfiguredFormats(const std::wstring& filter) {
    const Endpoint endpoint = SelectEndpoint(filter);
    ComPtr<IPolicyConfig> policy;
    ThrowIfFailed(CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&policy)),
                  "Create audio policy client");

    std::wcout << L"Configured formats for: " << endpoint.name << L"\n"
               << L"ID: " << endpoint.id << L"\n";

    WAVEFORMATEX* mixFormat = nullptr;
    const HRESULT mixResult = policy->GetMixFormat(endpoint.id.c_str(), &mixFormat);
    PrintPolicyFormat(L"mix", mixResult, mixFormat);

    WAVEFORMATEX* currentFormat = nullptr;
    const HRESULT currentResult =
        policy->GetDeviceFormat(endpoint.id.c_str(), FALSE, &currentFormat);
    PrintPolicyFormat(L"device current", currentResult, currentFormat);

    WAVEFORMATEX* defaultFormat = nullptr;
    const HRESULT defaultResult =
        policy->GetDeviceFormat(endpoint.id.c_str(), TRUE, &defaultFormat);
    PrintPolicyFormat(L"device default", defaultResult, defaultFormat);
}

namespace {

void PrintConnectorJackInfo(IConnector* connector, const wchar_t* label) {
    ConnectorType type{};
    DataFlow flow{};
    BOOL connected = FALSE;
    const HRESULT typeResult = connector->GetType(&type);
    const HRESULT flowResult = connector->GetDataFlow(&flow);
    const HRESULT connectedResult = connector->IsConnected(&connected);

    std::wcout << L"  " << label << L":\n"
               << L"    type: "
               << (SUCCEEDED(typeResult) ? std::to_wstring(static_cast<UINT32>(type))
                                         : HResultText(typeResult))
               << L"\n"
               << L"    flow: "
               << (SUCCEEDED(flowResult) ? std::to_wstring(static_cast<UINT32>(flow))
                                         : HResultText(flowResult))
               << L"\n"
               << L"    connected: ";
    if (SUCCEEDED(connectedResult)) {
        std::wcout << (connected ? L"yes" : L"no") << L"\n";
    } else {
        std::wcout << HResultText(connectedResult) << L"\n";
    }

    ComPtr<IPart> part;
    const HRESULT partResult = connector->QueryInterface(IID_PPV_ARGS(&part));
    if (FAILED(partResult)) {
        std::wcout << L"    IPart: " << HResultText(partResult) << L"\n";
        return;
    }

    ComPtr<IKsJackDescription> jackDescription;
    const HRESULT jackResult = part->Activate(
        CLSCTX_ALL, __uuidof(IKsJackDescription),
        reinterpret_cast<void**>(jackDescription.GetAddressOf()));
    if (SUCCEEDED(jackResult)) {
        UINT count = 0;
        const HRESULT countResult = jackDescription->GetJackCount(&count);
        std::wcout << L"    jack descriptions: ";
        if (FAILED(countResult)) {
            std::wcout << HResultText(countResult) << L"\n";
        } else {
            std::wcout << count << L"\n";
            for (UINT index = 0; index < count; ++index) {
                KSJACK_DESCRIPTION description{};
                const HRESULT descriptionResult =
                    jackDescription->GetJackDescription(index, &description);
                if (FAILED(descriptionResult)) {
                    std::wcout << L"      [" << index << L"] "
                               << HResultText(descriptionResult) << L"\n";
                    continue;
                }
                std::wcout << L"      [" << index << L"] channels=0x" << std::hex
                           << std::uppercase << description.ChannelMapping
                           << L", color=0x" << description.Color << std::dec
                           << L", connection=" << static_cast<UINT32>(description.ConnectionType)
                           << L", geo=" << static_cast<UINT32>(description.GeoLocation)
                           << L", general=" << static_cast<UINT32>(description.GenLocation)
                           << L", port=" << static_cast<UINT32>(description.PortConnection)
                           << L", connected=" << (description.IsConnected ? L"yes" : L"no")
                           << L"\n";
            }
        }
    } else {
        std::wcout << L"    IKsJackDescription: " << HResultText(jackResult) << L"\n";
    }

    ComPtr<IKsJackSinkInformation> sinkInformation;
    const HRESULT sinkResult = part->Activate(
        CLSCTX_ALL, __uuidof(IKsJackSinkInformation),
        reinterpret_cast<void**>(sinkInformation.GetAddressOf()));
    if (SUCCEEDED(sinkResult)) {
        KSJACK_SINK_INFORMATION sink{};
        const HRESULT informationResult = sinkInformation->GetJackSinkInformation(&sink);
        if (SUCCEEDED(informationResult)) {
            std::wcout << L"    sink: connection=" << static_cast<UINT32>(sink.ConnType)
                       << L", manufacturer=0x" << std::hex << std::uppercase
                       << sink.ManufacturerId << L", product=0x" << sink.ProductId
                       << std::dec << L", latency=" << sink.AudioLatency
                       << L" ms, HDCP=" << (sink.HDCPCapable ? L"yes" : L"no")
                       << L", AI=" << (sink.AICapable ? L"yes" : L"no") << L"\n"
                       << L"      description: " << sink.SinkDescription << L"\n"
                       << L"      port LUID: 0x" << std::hex << std::uppercase
                       << static_cast<UINT32>(sink.PortId.HighPart) << L":"
                       << sink.PortId.LowPart << std::dec << L"\n";
        } else {
            std::wcout << L"    GetJackSinkInformation: "
                       << HResultText(informationResult) << L"\n";
        }
    } else {
        std::wcout << L"    IKsJackSinkInformation: " << HResultText(sinkResult) << L"\n";
    }
}

} // namespace

void PrintEndpointJackInfo(const std::wstring& filter) {
    const Endpoint endpoint = SelectEndpoint(filter);
    std::wcout << L"Endpoint topology: " << endpoint.name << L"\n"
               << L"ID: " << endpoint.id << L"\n";

    ComPtr<IDeviceTopology> topology;
    ThrowIfFailed(endpoint.device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL,
                                             nullptr, &topology),
                  "Activate endpoint device topology");

    UINT connectorCount = 0;
    ThrowIfFailed(topology->GetConnectorCount(&connectorCount), "Get topology connector count");
    std::wcout << L"Connectors: " << connectorCount << L"\n";
    for (UINT index = 0; index < connectorCount; ++index) {
        ComPtr<IConnector> connector;
        ThrowIfFailed(topology->GetConnector(index, &connector), "Get topology connector");
        const std::wstring label = L"connector[" + std::to_wstring(index) + L"]";
        PrintConnectorJackInfo(connector.Get(), label.c_str());

        ComPtr<IConnector> peer;
        if (SUCCEEDED(connector->GetConnectedTo(&peer))) {
            const std::wstring peerLabel = label + L" peer";
            PrintConnectorJackInfo(peer.Get(), peerLabel.c_str());
        }
    }
}

WAVEFORMATEXTENSIBLE MakePcmFormat(const WORD channels, const DWORD channelMask) {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = channels;
    format.Format.nSamplesPerSec = 48000;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(std::int32_t));
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 24;
    format.dwChannelMask = channelMask;
    format.SubFormat = kPcm;
    return format;
}

Iec61937WaveFormat MakeIec61937Format(
    const GUID& subformat, const DWORD encodedSamplesPerSec,
    const DWORD channelMask) {
    Iec61937WaveFormat format{};
    format.formatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.formatExt.Format.nChannels = 8;
    format.formatExt.Format.nSamplesPerSec = 192000;
    format.formatExt.Format.nAvgBytesPerSec = 3072000;
    format.formatExt.Format.nBlockAlign = 16;
    format.formatExt.Format.wBitsPerSample = 16;
    format.formatExt.Format.cbSize = sizeof(Iec61937WaveFormat) - sizeof(WAVEFORMATEX);
    format.formatExt.Samples.wValidBitsPerSample = 16;
    format.formatExt.dwChannelMask = channelMask;
    format.formatExt.SubFormat = subformat;
    format.encodedSamplesPerSec = encodedSamplesPerSec;
    format.encodedChannelCount = 8;
    format.averageBytesPerSec = 0;
    return format;
}

Iec61937WaveFormat MakeDolbyDigitalPlusFormat(const GUID& subformat) {
    Iec61937WaveFormat format{};
    format.formatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.formatExt.Format.nChannels = 2;
    format.formatExt.Format.nSamplesPerSec = 192000;
    format.formatExt.Format.nAvgBytesPerSec = 768000;
    format.formatExt.Format.nBlockAlign = 4;
    format.formatExt.Format.wBitsPerSample = 16;
    format.formatExt.Format.cbSize = sizeof(Iec61937WaveFormat) - sizeof(WAVEFORMATEX);
    format.formatExt.Samples.wValidBitsPerSample = 16;
    format.formatExt.dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
    format.formatExt.SubFormat = subformat;
    format.encodedSamplesPerSec = 48000;
    format.encodedChannelCount = 6;
    format.averageBytesPerSec = 0;
    return format;
}

std::wstring FormatSupportText(IAudioClient* client, const WAVEFORMATEX* format,
                               const AUDCLNT_SHAREMODE mode) {
    WAVEFORMATEX* closest = nullptr;
    const HRESULT result = client->IsFormatSupported(mode, format,
                                                      mode == AUDCLNT_SHAREMODE_SHARED ? &closest : nullptr);
    std::wstring output;
    if (result == S_OK) {
        output = L"exact";
    } else if (result == S_FALSE && closest != nullptr) {
        output = L"conversion -> " + WaveFormatText(closest);
    } else {
        output = L"no (" + HResultText(result) + L")";
    }
    CoTaskMemFree(closest);
    return output;
}

void PrintFormatProbe(IAudioClient* client, const std::wstring_view name,
                      const WAVEFORMATEX* format) {
    std::wcout << L"    " << name << L"\n"
               << L"      shared:    "
               << FormatSupportText(client, format, AUDCLNT_SHAREMODE_SHARED) << L"\n"
               << L"      exclusive: "
               << FormatSupportText(client, format, AUDCLNT_SHAREMODE_EXCLUSIVE) << L"\n";
}

void PrintEndpoint(const Endpoint& endpoint) {
    std::wcout << L"\n" << (endpoint.isDefault ? L"* " : L"  ") << endpoint.name << L"\n"
               << L"    ID: " << endpoint.id << L"\n";

    ComPtr<IPropertyStore> properties;
    ThrowIfFailed(endpoint.device->OpenPropertyStore(STGM_READ, &properties),
                  "Open endpoint properties");
    UINT32 value = 0;
    if (ReadUintProperty(properties.Get(), PKEY_AudioEndpoint_FormFactor, value)) {
        std::wcout << L"    Form factor: " << FormFactorName(value) << L"\n";
    }
    if (ReadUintProperty(properties.Get(), PKEY_AudioEndpoint_PhysicalSpeakers, value)) {
        std::wcout << L"    Physical speakers: 0x" << std::hex << std::uppercase << value
                   << std::dec << L"\n";
    }

    ComPtr<IAudioClient> client;
    const HRESULT activation = endpoint.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                                          nullptr, &client);
    if (FAILED(activation)) {
        std::wcout << L"    IAudioClient: " << HResultText(activation) << L"\n";
        return;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mixFormat))) {
        std::wcout << L"    Mix format: " << WaveFormatText(mixFormat) << L"\n";
        CoTaskMemFree(mixFormat);
    }

    const auto pcm20 = MakePcmFormat(2, KSAUDIO_SPEAKER_STEREO);
    const auto pcm51 = MakePcmFormat(6, KSAUDIO_SPEAKER_5POINT1);
    const auto pcm71 = MakePcmFormat(8, KSAUDIO_SPEAKER_7POINT1_SURROUND);
    auto pcm714 = MakePcmFormat(12, 0x0002D63F);
    pcm714.Format.wBitsPerSample = 16;
    pcm714.Format.nBlockAlign = 24;
    pcm714.Format.nAvgBytesPerSec = 1'152'000;
    pcm714.Samples.wValidBitsPerSample = 16;
    const auto ddp = MakeDolbyDigitalPlusFormat(kDolbyDigitalPlus);
    const auto ddpAtmos = MakeDolbyDigitalPlusFormat(kDolbyDigitalPlusAtmos);
    const auto mat10 = MakeIec61937Format(kDolbyMlpMat10);
    auto mat10NoMask = mat10;
    mat10NoMask.formatExt.dwChannelMask = 0;
    const auto mat20_legacy =
        MakeIec61937Format(kDolbyMat20, 48000, KSAUDIO_SPEAKER_7POINT1);
    const auto mat20_48 = MakeIec61937Format(kDolbyMat20, 48000);
    const auto mat20_96 = MakeIec61937Format(kDolbyMat20, 96000);
    const auto mat20_192 = MakeIec61937Format(kDolbyMat20, 192000);
    const auto mat21p3 = MakeIec61937Format(kDolbyMat21Profile3);
    const auto mat21p4 = MakeIec61937Format(kDolbyMat21Profile4);
    const auto dtsHd = MakeIec61937Format(kDtsHd);
    const auto dtsXE1 = MakeIec61937Format(kDtsXE1);
    const auto dtsXE2 = MakeIec61937Format(kDtsXE2);

    std::wcout << L"    Format negotiation:\n";
    PrintFormatProbe(client.Get(), L"PCM 2.0 / 48 kHz / 24-in-32", &pcm20.Format);
    PrintFormatProbe(client.Get(), L"PCM 5.1 / 48 kHz / 24-in-32", &pcm51.Format);
    PrintFormatProbe(client.Get(), L"PCM 7.1 / 48 kHz / 24-in-32", &pcm71.Format);
    PrintFormatProbe(client.Get(), L"PCM 7.1.4 / 48 kHz / 16 bit", &pcm714.Format);
    PrintFormatProbe(client.Get(), L"Dolby Digital Plus", &ddp.formatExt.Format);
    PrintFormatProbe(client.Get(), L"Dolby Atmos (DD+)", &ddpAtmos.formatExt.Format);
    PrintFormatProbe(client.Get(), L"Dolby MLP / MAT 1.0", &mat10.formatExt.Format);
    PrintFormatProbe(client.Get(), L"Dolby MLP / MAT 1.0 / no channel mask",
                     &mat10NoMask.formatExt.Format);
    PrintFormatProbe(client.Get(), L"Dolby MAT 2.0 / legacy 7.1 mask",
                     &mat20_legacy.formatExt.Format);
    PrintFormatProbe(client.Get(), L"Dolby MAT 2.0 / 7.1 surround mask",
                     &mat20_48.formatExt.Format);
    PrintFormatProbe(client.Get(), L"Dolby MAT 2.0 / content 96 kHz", &mat20_96.formatExt.Format);
    PrintFormatProbe(client.Get(), L"Dolby MAT 2.0 / content 192 kHz", &mat20_192.formatExt.Format);
    PrintFormatProbe(client.Get(), L"Dolby MAT 2.1 Profile 3", &mat21p3.formatExt.Format);
    PrintFormatProbe(client.Get(), L"Dolby MAT 2.1 Profile 4", &mat21p4.formatExt.Format);
    PrintFormatProbe(client.Get(), L"DTS-HD", &dtsHd.formatExt.Format);
    PrintFormatProbe(client.Get(), L"DTS:X E1", &dtsXE1.formatExt.Format);
    PrintFormatProbe(client.Get(), L"DTS:X E2", &dtsXE2.formatExt.Format);

    ComPtr<ISpatialAudioClient> spatialClient;
    const HRESULT spatialResult = endpoint.device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL,
                                                             nullptr, &spatialClient);
    if (FAILED(spatialResult)) {
        std::wcout << L"    Spatial client: unavailable (" << HResultText(spatialResult) << L")\n";
        return;
    }

    AudioObjectType staticMask = AudioObjectType_None;
    UINT32 dynamicObjects = 0;
    const HRESULT maskResult = spatialClient->GetNativeStaticObjectTypeMask(&staticMask);
    const HRESULT dynamicResult = spatialClient->GetMaxDynamicObjectCount(&dynamicObjects);
    std::wcout << L"    Spatial client: available\n";
    if (SUCCEEDED(maskResult)) {
        std::wcout << L"      native static mask: 0x" << std::hex << std::uppercase
                   << static_cast<UINT32>(staticMask) << std::dec << L"\n";
    }
    if (SUCCEEDED(dynamicResult)) {
        std::wcout << L"      max dynamic objects: " << dynamicObjects << L"\n";
    }
}

bool IsFloatObjectFormat(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return format->wBitsPerSample == 32;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return format->wBitsPerSample == 32 && IsEqualGUID(extensible->SubFormat, kIeeeFloat);
}

} // namespace dolby
