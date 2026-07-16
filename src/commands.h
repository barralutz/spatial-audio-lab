#pragma once

#include "audio_platform.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace dolby {

void RenderTransportTest(double seconds, const std::wstring& filter,
                         const std::wstring& mode);
void SpatialSignalTest(double seconds, const std::wstring& filter,
                       const std::wstring& mode);
void CaptureLoopback(double seconds, const std::wstring& filter,
                     const std::filesystem::path& outputPath);
void CaptureProcessLoopback(double seconds, DWORD processId,
                            const std::filesystem::path& outputPath);

void AnalyzeFloatWave(const std::filesystem::path& inputPath);
void AnalyzeMatWave(const std::filesystem::path& inputPath);
void AnalyzeMatLayout(const std::filesystem::path& inputPath,
                      const std::filesystem::path& layoutPath);
void AnalyzeMatPositionTimeline(const std::filesystem::path& inputPath);
void CompareMatPositionFixtures(const std::array<std::filesystem::path, 6>& inputPaths);
void ExtractMat712Wave(const std::filesystem::path& inputPath,
                       const std::filesystem::path& outputPath);
void PlayAnalog712(const std::filesystem::path& inputPath,
                   const std::wstring& rearFilter,
                   const std::wstring& heightFilter,
                   double gain,
                   std::uint64_t repeatCount);
void PlayLiveMat712(double seconds,
                    const std::wstring& rearFilter,
                    const std::wstring& heightFilter,
                    double gain,
                    DWORD prebufferMilliseconds);
void PlayLiveMatLayout(double seconds,
                       const std::filesystem::path& layoutPath,
                       double gain,
                       DWORD prebufferMilliseconds);

} // namespace dolby
