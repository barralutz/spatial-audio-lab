#pragma once

#include "audio_platform.h"

#include "../driver/windows-driver-samples/audio/sysvad/matcapture.h"

#include <filesystem>
#include <vector>

namespace dolby {

struct MatCaptureReadView {
    MAT_CAPTURE_READ_HEADER header{};
    const BYTE* payload{};
};

MAT_CAPTURE_STATS QueryMatCaptureStats(HANDLE device);
WinHandle OpenMatCaptureDevice();
void ResetMatCapture(HANDLE device);
MatCaptureReadView ReadMatCapture(HANDLE device, std::vector<BYTE>& request);
void CaptureMatRing(double seconds, const std::filesystem::path& outputPath,
                    DWORD pollMilliseconds);

} // namespace dolby
