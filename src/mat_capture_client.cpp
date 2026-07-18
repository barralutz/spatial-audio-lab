#include "mat_capture_client.h"

#include "wave_io.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace dolby {

namespace {

struct MatCaptureReadHeaderV2 {
    ULONG version;
    ULONG headerBytes;
    ULONG payloadBytes;
    ULONG reserved;
    ULONGLONG firstByteSequence;
    ULONGLONG totalBytesWritten;
    ULONGLONG droppedBytes;
    GUID subFormat;
    ULONGLONG formatChanges;
};

struct MatCaptureStatsV2 {
    ULONG version;
    ULONG capacityBytes;
    ULONG availableBytes;
    ULONG reserved;
    ULONGLONG totalBytesWritten;
    ULONGLONG totalBytesRead;
    ULONGLONG droppedBytes;
    GUID subFormat;
    ULONGLONG formatChanges;
};

static_assert(offsetof(MAT_CAPTURE_READ_HEADER, SampleRate) == sizeof(MatCaptureReadHeaderV2));
static_assert(offsetof(MAT_CAPTURE_STATS, SampleRate) == sizeof(MatCaptureStatsV2));

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

void PopulateLegacyFormat(const GUID& subFormat, MAT_CAPTURE_STATS& stats) {
    if (IsEqualGUID(subFormat, GUID_NULL)) return;
    const Iec61937WaveFormat format = CarrierFormatForSubFormat(subFormat);
    stats.SampleRate = format.formatExt.Format.nSamplesPerSec;
    stats.ChannelMask = format.formatExt.dwChannelMask;
    stats.Channels = format.formatExt.Format.nChannels;
    stats.BitsPerSample = format.formatExt.Format.wBitsPerSample;
    stats.ValidBitsPerSample = format.formatExt.Samples.wValidBitsPerSample;
    stats.BlockAlign = format.formatExt.Format.nBlockAlign;
}

void PopulateLegacyFormat(const GUID& subFormat, MAT_CAPTURE_READ_HEADER& header) {
    MAT_CAPTURE_STATS stats{};
    PopulateLegacyFormat(subFormat, stats);
    header.SampleRate = stats.SampleRate;
    header.ChannelMask = stats.ChannelMask;
    header.Channels = stats.Channels;
    header.BitsPerSample = stats.BitsPerSample;
    header.ValidBitsPerSample = stats.ValidBitsPerSample;
    header.BlockAlign = stats.BlockAlign;
}

} // namespace

MAT_CAPTURE_STATS QueryMatCaptureStats(const HANDLE device) {
    alignas(MAT_CAPTURE_STATS) std::array<BYTE, sizeof(MAT_CAPTURE_STATS)> response{};
    DWORD returned = 0;
    if (!DeviceIoControl(device, IOCTL_MAT_CAPTURE_GET_STATS, nullptr, 0,
                         response.data(), static_cast<DWORD>(response.size()),
                         &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_GET_STATS failed (Win32 " << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
    if (returned < sizeof(ULONG)) {
        throw std::runtime_error("Capture driver returned truncated stats");
    }
    const ULONG version = *reinterpret_cast<const ULONG*>(response.data());
    if (version == MAT_CAPTURE_PROTOCOL_VERSION && returned == sizeof(MAT_CAPTURE_STATS)) {
        return *reinterpret_cast<const MAT_CAPTURE_STATS*>(response.data());
    }
    if (version == MAT_CAPTURE_PROTOCOL_VERSION_IEC61937 &&
        returned == sizeof(MatCaptureStatsV2)) {
        const auto& legacy = *reinterpret_cast<const MatCaptureStatsV2*>(response.data());
        MAT_CAPTURE_STATS stats{};
        stats.Version = legacy.version;
        stats.CapacityBytes = legacy.capacityBytes;
        stats.AvailableBytes = legacy.availableBytes;
        stats.Reserved = legacy.reserved;
        stats.TotalBytesWritten = legacy.totalBytesWritten;
        stats.TotalBytesRead = legacy.totalBytesRead;
        stats.DroppedBytes = legacy.droppedBytes;
        stats.SubFormat = legacy.subFormat;
        stats.FormatChanges = legacy.formatChanges;
        PopulateLegacyFormat(stats.SubFormat, stats);
        return stats;
    }
    {
        std::ostringstream message;
        message << "IEC 61937 capture driver returned incompatible stats (version="
                << version << ", bytes=" << returned << ", expected version="
                << MAT_CAPTURE_PROTOCOL_VERSION << ", expected bytes="
                << sizeof(MAT_CAPTURE_STATS) << ')';
        throw std::runtime_error(message.str());
    }
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
    if (returned < sizeof(ULONG)) {
        throw std::runtime_error("Capture driver returned a truncated read header");
    }

    const ULONG version = *reinterpret_cast<const ULONG*>(request.data());
    if (version == MAT_CAPTURE_PROTOCOL_VERSION) {
        if (returned < sizeof(MAT_CAPTURE_READ_HEADER)) {
            throw std::runtime_error("Capture driver returned a truncated v3 read header");
        }
        const auto* header = reinterpret_cast<const MAT_CAPTURE_READ_HEADER*>(request.data());
        if (header->HeaderBytes < sizeof(MAT_CAPTURE_READ_HEADER) ||
            static_cast<std::uint64_t>(header->HeaderBytes) + header->PayloadBytes > returned) {
            throw std::runtime_error("Capture driver returned an invalid v3 read result");
        }
        return {*header, request.data() + header->HeaderBytes};
    }
    if (version == MAT_CAPTURE_PROTOCOL_VERSION_IEC61937) {
        if (returned < sizeof(MatCaptureReadHeaderV2)) {
            throw std::runtime_error("Capture driver returned a truncated v2 read header");
        }
        const auto& legacy = *reinterpret_cast<const MatCaptureReadHeaderV2*>(request.data());
        if (legacy.headerBytes < sizeof(MatCaptureReadHeaderV2) ||
            static_cast<std::uint64_t>(legacy.headerBytes) + legacy.payloadBytes > returned) {
            throw std::runtime_error("Capture driver returned an invalid v2 read result");
        }
        MAT_CAPTURE_READ_HEADER header{};
        header.Version = legacy.version;
        header.HeaderBytes = legacy.headerBytes;
        header.PayloadBytes = legacy.payloadBytes;
        header.Reserved = legacy.reserved;
        header.FirstByteSequence = legacy.firstByteSequence;
        header.TotalBytesWritten = legacy.totalBytesWritten;
        header.DroppedBytes = legacy.droppedBytes;
        header.SubFormat = legacy.subFormat;
        header.FormatChanges = legacy.formatChanges;
        PopulateLegacyFormat(header.SubFormat, header);
        return {header, request.data() + legacy.headerBytes};
    }
    throw std::runtime_error("Capture driver returned an unsupported protocol version");
}

void ResetDriverFormatLog() {
    WinHandle device = OpenMatCaptureDevice();
    DWORD returned = 0;
    if (!DeviceIoControl(device.Get(), IOCTL_MAT_CAPTURE_RESET_FORMAT_LOG, nullptr, 0,
                         nullptr, 0, &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_RESET_FORMAT_LOG failed (Win32 "
                << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
    std::wcout << L"Driver format log reset.\n";
}

void PrintDriverFormatLog() {
    constexpr std::size_t recordCapacity = 512;
    WinHandle device = OpenMatCaptureDevice();
    std::vector<BYTE> response(
        sizeof(MAT_CAPTURE_FORMAT_LOG) + recordCapacity * sizeof(MAT_CAPTURE_FORMAT_EVENT));
    DWORD returned = 0;
    if (!DeviceIoControl(device.Get(), IOCTL_MAT_CAPTURE_GET_FORMAT_LOG, nullptr, 0,
                         response.data(), static_cast<DWORD>(response.size()),
                         &returned, nullptr)) {
        std::ostringstream message;
        message << "IOCTL_MAT_CAPTURE_GET_FORMAT_LOG failed (Win32 "
                << GetLastError() << ')';
        throw std::runtime_error(message.str());
    }
    if (returned < sizeof(MAT_CAPTURE_FORMAT_LOG)) {
        throw std::runtime_error("Capture driver returned a truncated format log");
    }

    const auto& log = *reinterpret_cast<const MAT_CAPTURE_FORMAT_LOG*>(response.data());
    if (log.Version != MAT_CAPTURE_FORMAT_LOG_VERSION ||
        log.HeaderBytes != sizeof(MAT_CAPTURE_FORMAT_LOG) ||
        log.RecordBytes != sizeof(MAT_CAPTURE_FORMAT_EVENT) ||
        log.RecordCount > recordCapacity ||
        static_cast<std::uint64_t>(log.HeaderBytes) +
                static_cast<std::uint64_t>(log.RecordCount) * log.RecordBytes > returned) {
        throw std::runtime_error("Capture driver returned an incompatible format log");
    }

    const auto* records = reinterpret_cast<const MAT_CAPTURE_FORMAT_EVENT*>(
        response.data() + log.HeaderBytes);
    const ULONGLONG origin = log.RecordCount == 0 ? 0 : records[0].InterruptTime100ns;
    std::wcout << L"Driver format log: retained=" << log.RecordCount
               << L", total=" << log.TotalRecords
               << L", overwritten=" << log.DroppedRecords << L"\n";

    for (ULONG index = 0; index < log.RecordCount; ++index) {
        const auto& event = records[index];
        const wchar_t* type = event.Type == MatCaptureFormatEventIsFormatSupported
                                ? L"support"
                                : event.Type == MatCaptureFormatEventNewStream
                                    ? L"stream"
                                    : event.Type == MatCaptureFormatEventWritePacket
                                        ? L"packet"
                                    : L"unknown";

        const double milliseconds = static_cast<double>(
            event.InterruptTime100ns - origin) / 10'000.0;
        if (event.Type == MatCaptureFormatEventWritePacket) {
            const ULONG lastPacket = static_cast<ULONG>(event.FormatTag) |
                (static_cast<ULONG>(event.Channels) << 16);
            std::wcout << L'#' << event.Sequence
                       << L" +" << std::fixed << std::setprecision(3) << milliseconds << L" ms"
                       << L" packet stream=0x" << std::hex << std::uppercase
                       << event.ProcessId
                       << L" number=" << std::dec << event.Pin;
            if (lastPacket == ULONG_MAX) {
                std::wcout << L" last=none";
            } else {
                std::wcout << L" last=" << lastPacket;
            }
            std::wcout << L" state=" << event.Capture
                       << L" status=0x" << std::hex << std::uppercase
                       << std::setw(8) << std::setfill(L'0')
                       << static_cast<std::uint32_t>(event.Status)
                       << std::dec << std::nouppercase << std::setfill(L' ')
                       << L" buffer=" << event.FormatSize
                       << L" notifications=" << event.ChannelMask
                       << L" packet-bytes=" << event.AverageBytesPerSecond
                       << L" interval=" << event.SampleRate << L" ms\n";
            continue;
        }

        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = event.FormatTag;
        format.Format.nChannels = event.Channels;
        format.Format.nSamplesPerSec = event.SampleRate;
        format.Format.nAvgBytesPerSec = event.AverageBytesPerSecond;
        format.Format.nBlockAlign = event.BlockAlign;
        format.Format.wBitsPerSample = event.BitsPerSample;
        format.Format.cbSize = event.ExtraSize;
        format.Samples.wValidBitsPerSample = event.ValidBitsPerSample;
        format.dwChannelMask = event.ChannelMask;
        format.SubFormat = event.WaveSubFormat;

        std::wcout << L'#' << event.Sequence
                   << L" +" << std::fixed << std::setprecision(3) << milliseconds << L" ms"
                   << L" " << type
                   << L" pid=" << event.ProcessId
                   << L" tid=" << event.ThreadId
                   << L" pin=" << event.Pin
                   << (event.Capture != 0 ? L" capture" : L" render")
                   << L" status=0x" << std::hex << std::uppercase
                   << std::setw(8) << std::setfill(L'0')
                   << static_cast<std::uint32_t>(event.Status)
                   << std::dec << std::nouppercase << std::setfill(L' ')
                   << L" " << WaveFormatText(&format.Format)
                   << L", avg=" << event.AverageBytesPerSecond << L" B/s";
        if (!IsEqualGUID(event.DataSubFormat, event.WaveSubFormat)) {
            std::wcout << L"; KS subtype=" << GuidText(event.DataSubFormat);
        }
        std::wcout << L"\n";
    }
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
