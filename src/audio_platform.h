#pragma once

#include <windows.h>

#include <audioclient.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <wrl/client.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dolby {

using Microsoft::WRL::ComPtr;

inline constexpr GUID kPcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kDolbyDigitalPlus = {
    0x0000000a, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kDolbyDigitalPlusAtmos = {
    0x0000010a, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kDolbyMlpMat10 = {
    0x0000000c, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kDolbyMat20 = {
    0x0000010c, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kDolbyMat21Profile3 = {
    0x0000030c, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kDolbyMat21Profile4 = {
    0x0000070c, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kDts = {
    0x00000008, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kDtsHd = {
    0x0000000b, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kDtsXE1 = {
    0x0000010b, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kDtsXE2 = {
    0x0000030b, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

struct Iec61937WaveFormat {
    WAVEFORMATEXTENSIBLE formatExt;
    DWORD encodedSamplesPerSec;
    DWORD encodedChannelCount;
    DWORD averageBytesPerSec;
};

class ComApartment {
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result)) throw std::runtime_error("CoInitializeEx failed");
    }

    ~ComApartment() { CoUninitialize(); }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

class WinHandle {
public:
    explicit WinHandle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~WinHandle() {
        if (IsValid()) CloseHandle(handle_);
    }

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            if (IsValid()) CloseHandle(handle_);
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    HANDLE Get() const { return handle_; }
    bool IsValid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

private:
    HANDLE handle_;
};

struct Endpoint {
    ComPtr<IMMDevice> device;
    std::wstring id;
    std::wstring name;
    bool isDefault{};
};

class AudioClientError : public std::runtime_error {
public:
    AudioClientError(HRESULT result, const char* operation);

    HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_;
};

std::wstring HResultText(HRESULT result);
std::wstring GuidText(const GUID& guid);
void ThrowIfFailed(HRESULT result, const char* operation);
bool IsRecoverableAudioClientError(HRESULT result);
std::wstring Lowercase(std::wstring value);
std::wstring WaveFormatText(const WAVEFORMATEX* format);
double Decibels(double linear);

std::vector<Endpoint> EnumerateRenderEndpoints();
Endpoint SelectEndpoint(const std::wstring& filter);
void SetDefaultEndpoint(const std::wstring& filter);
void SetNativePcm714Format(const std::wstring& filter);
void SetSpatialCodecFormat(const std::wstring& filter, bool dtsX);
void SetLegacyMatFormat(const std::wstring& filter);
void PrintConfiguredFormats(const std::wstring& filter);
void PrintEndpoint(const Endpoint& endpoint);
void PrintEndpointJackInfo(const std::wstring& filter);

WAVEFORMATEXTENSIBLE MakePcmFormat(WORD channels, DWORD channelMask);
Iec61937WaveFormat MakeIec61937Format(
    const GUID& subformat,
    DWORD encodedSamplesPerSec = 96000,
    DWORD channelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND);
Iec61937WaveFormat MakeDolbyDigitalPlusFormat(const GUID& subformat);
bool IsFloatObjectFormat(const WAVEFORMATEX* format);

} // namespace dolby
