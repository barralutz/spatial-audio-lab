#include "mat_capture_client.h"

#include "wave_io.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace dolby {

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
        throw std::runtime_error("MAT capture driver returned incompatible stats");
    }
    return stats;
}

WinHandle OpenMatCaptureDevice() {
    WinHandle device(CreateFileW(MAT_CAPTURE_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!device.IsValid()) {
        std::ostringstream message;
        message << "Could not open " << "\\\\.\\DolbyDecoderMat" << " (Win32 "
                << GetLastError() << "). Install and load the MAT capture driver as administrator.";
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
        throw std::runtime_error("MAT capture driver returned a truncated read header");
    }

    const auto* header = reinterpret_cast<const MAT_CAPTURE_READ_HEADER*>(request.data());
    if (header->Version != MAT_CAPTURE_PROTOCOL_VERSION ||
        header->HeaderBytes < sizeof(MAT_CAPTURE_READ_HEADER) ||
        static_cast<std::uint64_t>(header->HeaderBytes) + header->PayloadBytes > returned) {
        throw std::runtime_error("MAT capture driver returned an invalid read result");
    }
    return {*header, request.data() + header->HeaderBytes};
}

void CaptureMatRing(const double seconds, const std::filesystem::path& outputPath,
                    const DWORD pollMilliseconds) {
    WinHandle device = OpenMatCaptureDevice();
    ResetMatCapture(device.Get());

    const MAT_CAPTURE_STATS initialStats = QueryMatCaptureStats(device.Get());
    const auto carrierFormat = MakeIec61937Format(kDolbyMat21Profile3);
    WaveWriter writer(outputPath, &carrierFormat.formatExt.Format);
    constexpr std::size_t payloadCapacity = 256U * 1024U;
    std::vector<BYTE> request(sizeof(MAT_CAPTURE_READ_HEADER) + payloadCapacity);
    std::vector<BYTE> alignmentBuffer;
    std::array<BYTE, 3> scanTail{};
    std::size_t scanTailBytes = 0;
    std::uint64_t expectedSequence = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t capturedBytes = 0;
    std::uint64_t preambles = 0;

    std::wcout << L"MAT ring: capacity=" << initialStats.CapacityBytes
               << L" bytes, poll=" << pollMilliseconds << L" ms\n";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    bool draining = false;
    while (true) {
        const MatCaptureReadView read = ReadMatCapture(device.Get(), request);
        if (read.header.PayloadBytes != 0) {
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
                alignmentBuffer.size() - alignmentBuffer.size() % carrierFormat.formatExt.Format.nBlockAlign;
            if (completeBytes != 0) {
                writer.Write(alignmentBuffer.data(),
                             static_cast<UINT32>(completeBytes /
                                                 carrierFormat.formatExt.Format.nBlockAlign),
                             false);
                alignmentBuffer.erase(alignmentBuffer.begin(),
                                      alignmentBuffer.begin() + completeBytes);
            }
        }

        if (!draining && std::chrono::steady_clock::now() >= deadline) draining = true;
        if (draining && read.header.PayloadBytes == 0) break;
        if (read.header.PayloadBytes == 0) Sleep(pollMilliseconds);
    }

    writer.Finalize();
    const MAT_CAPTURE_STATS finalStats = QueryMatCaptureStats(device.Get());
    std::wcout << L"MAT ring capture complete: " << outputPath.wstring() << L"\n"
               << L"Bytes=" << capturedBytes << L", IEC preambles=" << preambles
               << L", sequence gaps=" << sequenceGaps << L"\n"
               << L"Driver written=" << finalStats.TotalBytesWritten
               << L", read=" << finalStats.TotalBytesRead
               << L", dropped=" << finalStats.DroppedBytes
               << L", trailing partial=" << alignmentBuffer.size() << L" bytes\n";
}

} // namespace dolby
