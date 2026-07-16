#include "mat_capture_client.h"

#include "wave_io.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace dolby {

namespace {

Iec61937WaveFormat CarrierFormatForSubFormat(const GUID& subFormat) {
    if (IsEqualGUID(subFormat, kDts)) {
        Iec61937WaveFormat format = MakeIec61937Format(kDts, 48000,
                                                       KSAUDIO_SPEAKER_5POINT1_SURROUND);
        format.formatExt.Format.nChannels = 2;
        format.formatExt.Format.nSamplesPerSec = 48000;
        format.formatExt.Format.nAvgBytesPerSec = 192000;
        format.formatExt.Format.nBlockAlign = 4;
        format.encodedChannelCount = 6;
        return format;
    }
    if (IsEqualGUID(subFormat, kDtsHd) || IsEqualGUID(subFormat, kDtsXE1) ||
        IsEqualGUID(subFormat, kDtsXE2) || IsEqualGUID(subFormat, kDolbyMlpMat10) ||
        IsEqualGUID(subFormat, kDolbyMat20) ||
        IsEqualGUID(subFormat, kDolbyMat21Profile3)) {
        return MakeIec61937Format(subFormat);
    }
    throw std::runtime_error("The capture driver reported an unsupported IEC 61937 subtype");
}

} // namespace

MAT_CAPTURE_STATS QueryMatCaptureStats(const HANDLE device) {
    MAT_CAPTURE_STATS stats{};
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_MAT_CAPTURE_GET_STATS, nullptr, 0,
                         &stats, sizeof(stats), &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_GET_STATS failed (Win32 " << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
    if (returned != sizeof(stats) || stats.Version != MAT_CAPTURE_PROTOCOL_VERSION) {
        std::ostringstream message;
        message << "IEC 61937 capture driver returned incompatible stats (version="
                << stats.Version << ", bytes=" << returned << ", expected version="
                << MAT_CAPTURE_PROTOCOL_VERSION << ", expected bytes=" << sizeof(stats) << ')';
        throw std::runtime_error(message.str());
    }
    return stats;
}

WinHandle OpenMatCaptureDevice() {
    WinHandle device(CreateFileW(MAT_CAPTURE_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!device.IsValid()) {
        std::ostringstream message;
        message << "Could not open " << "\\\\.\\DolbyDecoderMat" << " (Win32 "
                << GetLastError() << "). Install and load the capture driver as administrator.";
        throw std::runtime_error(message.str());
    }
    return device;
}

void ResetMatCapture(const HANDLE device) {
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_MAT_CAPTURE_RESET, nullptr, 0,
                         nullptr, 0, &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_RESET failed (Win32 " << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
}

MatCaptureReadView ReadMatCapture(const HANDLE device, std::vector<BYTE>& request) {
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_MAT_CAPTURE_READ, nullptr, 0,
                         request.data(), static_cast<DWORD>(request.size()), &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_READ failed (Win32 " << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
    if (returned < sizeof(MAT_CAPTURE_READ_HEADER)) {
        throw std::runtime_error("IEC 61937 capture driver returned a truncated read header");
    }

    const auto* header = reinterpret_cast<const MAT_CAPTURE_READ_HEADER*>(request.data());
    if (header->Version != MAT_CAPTURE_PROTOCOL_VERSION ||
        header->HeaderBytes < sizeof(MAT_CAPTURE_READ_HEADER) ||
        static_cast<std::uint64_t>(header->HeaderBytes) + header->PayloadBytes > returned) {
        throw std::runtime_error("IEC 61937 capture driver returned an invalid read result");
    }
    return {*header, request.data() + header->HeaderBytes};
}

void CaptureIec61937Ring(const double seconds, const std::filesystem::path& outputPath,
                         const DWORD pollMilliseconds) {
    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());

    const MAT_CAPTURE_STATS initialStats = QueryMatCaptureStats(device.Get());
    std::unique_ptr<WaveWriter> writer;
    GUID capturedSubFormat = GUID_NULL;
    WORD blockAlign = 0;
    constexpr std::size_t payloadCapacity = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + payloadCapacity);
    std::vector<BYTE> alignmentBuffer;
    std::array<BYTE, 3> scanTail{};
    std::size_t scanTailBytes = 0;
    std::uint64_t expectedSequence = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t capturedBytes = 0;
    std::uint64_t preambles = 0;

    std::wcout << L"IEC 61937 ring: capacity=" << initialStats.CapacityBytes
               << L" bytes, poll=" << pollMilliseconds << L" ms\n";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    bool draining = false;
    while (true) {
        const MatCaptureReadView read = ReadMatCapture(device.Get(), request);
        if (read.header.PayloadBytes != 0) {
            if (IsEqualGUID(read.header.SubFormat, GUID_NULL)) {
                throw std::runtime_error("Captured bytes arrived without an IEC 61937 subtype");
            }
            if (IsEqualGUID(capturedSubFormat, GUID_NULL)) {
                capturedSubFormat = read.header.SubFormat;
                const Iec61937WaveFormat carrierFormat =
                    CarrierFormatForSubFormat(capturedSubFormat);
                blockAlign = carrierFormat.formatExt.Format.nBlockAlign;
                writer = std::make_unique<WaveWriter>(outputPath,
                                                       &carrierFormat.formatExt.Format);
                std::wcout << L"Carrier: "
                           << WaveFormatText(&carrierFormat.formatExt.Format) << L"\n";
            } else if (!IsEqualGUID(capturedSubFormat, read.header.SubFormat) ||
                       read.header.FormatChanges != 0) {
                throw std::runtime_error("IEC 61937 subtype changed during capture");
            }
            if (capturedBytes != 0 && read.header.FirstByteSequence != expectedSequence) {
                sequenceGaps += read.header.FirstByteSequence > expectedSequence
                                    ? read.header.FirstByteSequence - expectedSequence
                                    : expectedSequence - read.header.FirstByteSequence;
            }
            expectedSequence = read.header.FirstByteSequence + read.header.PayloadBytes;
            capturedBytes += read.header.PayloadBytes;

            std::vector<BYTE> scan(scanTailBytes + read.header.PayloadBytes);
            std::copy_n(scanTail.data(), scanTailBytes, scan.begin());
            std::copy_n(read.payload, read.header.PayloadBytes, scan.begin() + scanTailBytes);
            preambles += CountIec61937Preambles(scan.data(), scan.size());
            scanTailBytes = std::min<std::size_t>(scanTail.size(), scan.size());
            std::copy(scan.end() - scanTailBytes, scan.end(), scanTail.begin());

            alignmentBuffer.insert(alignmentBuffer.end(), read.payload,
                                   read.payload + read.header.PayloadBytes);
            const std::size_t completeBytes =
                alignmentBuffer.size() - alignmentBuffer.size() % blockAlign;
            if (completeBytes != 0) {
                writer->Write(alignmentBuffer.data(),
                              static_cast<UINT32>(completeBytes / blockAlign), false);
                alignmentBuffer.erase(alignmentBuffer.begin(),
                                      alignmentBuffer.begin() + completeBytes);
            }
        }

        if (!draining && std::chrono::steady_clock::now() >= deadline) draining = true;
        if (draining && read.header.PayloadBytes == 0) break;
        if (read.header.PayloadBytes == 0) Sleep(pollMilliseconds);
    }

    if (!writer) {
        throw std::runtime_error(
            "No IEC 61937 bytes arrived. Start a spatial or encoded-audio source during capture.");
    }
    writer->Finalize();
    const MAT_CAPTURE_STATS finalStats = QueryMatCaptureStats(device.Get());
    std::wcout << L"IEC 61937 ring capture complete: " << outputPath.wstring() << L"\n"
               << L"Bytes=" << capturedBytes << L", IEC preambles=" << preambles
               << L", sequence gaps=" << sequenceGaps << L"\n"
               << L"Driver written=" << finalStats.TotalBytesWritten
               << L", read=" << finalStats.TotalBytesRead
               << L", dropped=" << finalStats.DroppedBytes
               << L", trailing partial=" << alignmentBuffer.size() << L" bytes\n";
}

void CaptureMatRing(const double seconds, const std::filesystem::path& outputPath,
                    const DWORD pollMilliseconds) {
    CaptureIec61937Ring(seconds, outputPath, pollMilliseconds);
}

} // namespace dolby
