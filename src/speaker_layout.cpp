#include "speaker_layout.h"

#include "audio_platform.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <set>
#include <stdexcept>

namespace dolby {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::wstring Trim(std::wstring value) {
    const auto whitespace = [](const wchar_t character) { return iswspace(character) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
    return value;
}

std::wstring ReadIniString(const std::filesystem::path& path,
                           const std::wstring& section,
                           const std::wstring& key,
                           const std::wstring& fallback = L"") {
    std::vector<wchar_t> buffer(32'768);
    const DWORD copied = GetPrivateProfileStringW(
        section.c_str(), key.c_str(), fallback.c_str(), buffer.data(),
        static_cast<DWORD>(buffer.size()), path.c_str());
    if (copied + 1 >= buffer.size()) {
        throw std::runtime_error("Speaker layout value exceeds the INI parser limit");
    }
    return Trim(std::wstring(buffer.data(), copied));
}

std::wstring ReadRequiredIniString(const std::filesystem::path& path,
                                   const std::wstring& section,
                                   const std::wstring& key) {
    const std::wstring value = ReadIniString(path, section, key);
    if (value.empty()) {
        throw std::runtime_error("Speaker layout is missing a required INI value");
    }
    return value;
}

std::vector<std::wstring> SplitList(const std::wstring& value) {
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t separator = value.find(L',', start);
        std::wstring part = Trim(value.substr(
            start, separator == std::wstring::npos ? std::wstring::npos : separator - start));
        if (part.empty()) throw std::runtime_error("Speaker layout contains an empty list item");
        parts.push_back(std::move(part));
        if (separator == std::wstring::npos) break;
        start = separator + 1;
    }
    return parts;
}

double ReadIniDouble(const std::filesystem::path& path,
                     const std::wstring& section,
                     const std::wstring& key,
                     const std::optional<double> fallback = std::nullopt) {
    std::wstring value = ReadIniString(path, section, key);
    if (value.empty()) {
        if (fallback.has_value()) return *fallback;
        throw std::runtime_error("Speaker layout is missing a required numeric INI value");
    }
    std::size_t consumed = 0;
    const double result = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(result)) {
        throw std::runtime_error("Speaker layout contains an invalid numeric value");
    }
    return result;
}

double NormalizeAzimuth(double degrees) {
    degrees = std::fmod(degrees + 180.0, 360.0);
    if (degrees < 0.0) degrees += 360.0;
    return degrees - 180.0;
}

void PanRing(const SpeakerLayout& layout,
             std::vector<std::size_t> indices,
             const double azimuthDegrees,
             const double scale,
             std::vector<double>& gains) {
    if (indices.empty() || scale == 0.0) return;
    std::sort(indices.begin(), indices.end(), [&](const std::size_t first,
                                                   const std::size_t second) {
        return NormalizeAzimuth(layout.speakers[first].azimuthDegrees) <
               NormalizeAzimuth(layout.speakers[second].azimuthDegrees);
    });
    if (indices.size() == 1) {
        gains[indices.front()] += scale;
        return;
    }

    double target = NormalizeAzimuth(azimuthDegrees);
    const double firstAngle = NormalizeAzimuth(layout.speakers[indices.front()].azimuthDegrees);
    if (target < firstAngle) target += 360.0;
    for (std::size_t index = 0; index < indices.size(); ++index) {
        const std::size_t next = (index + 1) % indices.size();
        const double from = NormalizeAzimuth(layout.speakers[indices[index]].azimuthDegrees) +
                            (index == 0 ? 0.0 :
                             NormalizeAzimuth(layout.speakers[indices[index]].azimuthDegrees) <
                                     firstAngle
                                 ? 360.0
                                 : 0.0);
        double to = next == 0
                        ? firstAngle + 360.0
                        : NormalizeAzimuth(layout.speakers[indices[next]].azimuthDegrees);
        if (to <= from) to += 360.0;
        if (target < from || target > to) continue;
        const double fraction = std::clamp((target - from) / (to - from), 0.0, 1.0);
        gains[indices[index]] += scale * std::cos(fraction * kPi / 2.0);
        gains[indices[next]] += scale * std::sin(fraction * kPi / 2.0);
        return;
    }
    throw std::runtime_error("Speaker ring could not bracket an azimuth");
}

} // namespace

std::optional<std::size_t> SpeakerLayout::FindSpeaker(
    const std::wstring_view speakerName) const {
    const std::wstring needle = Lowercase(std::wstring(speakerName));
    for (std::size_t index = 0; index < speakers.size(); ++index) {
        if (Lowercase(speakers[index].name) == needle) return index;
    }
    return std::nullopt;
}

SpeakerLayout LoadSpeakerLayout(const std::filesystem::path& path) {
    const std::filesystem::path fullPath = std::filesystem::absolute(path);
    if (!std::filesystem::is_regular_file(fullPath)) {
        throw std::runtime_error("Speaker layout INI file was not found");
    }

    const std::wstring versionText = ReadIniString(fullPath, L"profile", L"version");
    const bool version2 = !versionText.empty();
    if (version2 && versionText != L"2") {
        throw std::runtime_error("Speaker layout uses an unsupported profile version");
    }

    SpeakerLayout layout;
    layout.name = ReadRequiredIniString(
        fullPath, version2 ? L"profile" : L"layout", L"name");
    const std::vector<std::wstring> speakerNames = SplitList(
        ReadRequiredIniString(fullPath, L"layout", L"speakers"));
    const std::vector<std::wstring> outputNames = SplitList(
        ReadRequiredIniString(fullPath, L"layout", L"outputs"));
    const std::wstring masterName = Lowercase(
        ReadRequiredIniString(fullPath, L"layout", L"master"));
    if (speakerNames.size() < 2 || speakerNames.size() > 12 ||
        outputNames.empty() || outputNames.size() > 16) {
        throw std::runtime_error("Speaker layout has an unsupported number of speakers or outputs");
    }

    std::set<std::wstring> uniqueSpeakerNames;
    for (const std::wstring& speakerName : speakerNames) {
        const std::wstring normalized = Lowercase(speakerName);
        if (!uniqueSpeakerNames.insert(normalized).second) {
            throw std::runtime_error("Speaker layout contains a duplicate speaker name");
        }
        const std::wstring section = L"speaker." + speakerName;
        SpeakerDefinition speaker;
        speaker.name = speakerName;
        speaker.azimuthDegrees = NormalizeAzimuth(
            ReadIniDouble(fullPath, section, L"azimuth"));
        speaker.elevationDegrees = ReadIniDouble(fullPath, section, L"elevation");
        speaker.trimDb = ReadIniDouble(fullPath, section, L"trim_db", 0.0);
        if (speaker.elevationDegrees < -90.0 || speaker.elevationDegrees > 90.0 ||
            speaker.trimDb < -60.0 || speaker.trimDb > 12.0) {
            throw std::runtime_error("Speaker position or trim lies outside the supported range");
        }
        layout.speakers.push_back(std::move(speaker));
    }

    std::vector<bool> assigned(layout.speakers.size());
    std::set<std::wstring> endpointFilters;
    bool foundMaster = false;
    for (const std::wstring& outputName : outputNames) {
        const std::wstring section = L"output." + outputName;
        OutputRouteDefinition output;
        output.name = outputName;
        if (version2) {
            output.endpointFilter = ReadIniString(fullPath, section, L"endpoint_id");
            if (output.endpointFilter.empty()) {
                output.endpointFilter = ReadRequiredIniString(
                    fullPath, section, L"endpoint_name");
            }
        } else {
            output.endpointFilter = ReadRequiredIniString(fullPath, section, L"endpoint");
        }
        output.delayMilliseconds = ReadIniDouble(fullPath, section, L"delay_ms", 0.0);
        if (output.delayMilliseconds < 0.0 || output.delayMilliseconds > 500.0) {
            throw std::runtime_error("Output delay must be between 0 and 500 milliseconds");
        }
        if (!endpointFilters.insert(Lowercase(output.endpointFilter)).second) {
            throw std::runtime_error("Each output route must use a distinct endpoint filter");
        }
        for (const std::wstring& speakerName : SplitList(
                 ReadRequiredIniString(fullPath, section, L"speakers"))) {
            const auto speaker = layout.FindSpeaker(speakerName);
            if (!speaker.has_value()) {
                throw std::runtime_error("Output route references an unknown speaker");
            }
            if (assigned[*speaker]) {
                throw std::runtime_error("A speaker cannot be assigned to more than one output");
            }
            assigned[*speaker] = true;
            output.speakerIndices.push_back(*speaker);
        }
        if (output.speakerIndices.empty()) {
            throw std::runtime_error("An output route cannot be empty");
        }
        if (version2) {
            const double expectedChannels = ReadIniDouble(
                fullPath, section, L"expected_channels");
            if (expectedChannels < 1.0 || expectedChannels > 32.0 ||
                std::floor(expectedChannels) != expectedChannels ||
                output.speakerIndices.size() >
                    static_cast<std::size_t>(expectedChannels)) {
                throw std::runtime_error(
                    "Output route exceeds its expected endpoint channel count");
            }
        }
        if (Lowercase(outputName) == masterName) {
            if (foundMaster) throw std::runtime_error("Speaker layout has multiple master outputs");
            layout.masterOutput = layout.outputs.size();
            foundMaster = true;
        }
        layout.outputs.push_back(std::move(output));
    }
    if (!foundMaster) throw std::runtime_error("Speaker layout master does not name an output");
    if (std::find(assigned.begin(), assigned.end(), false) != assigned.end()) {
        throw std::runtime_error("Every speaker must be assigned to exactly one output route");
    }
    return layout;
}

std::vector<double> PanDirection(const SpeakerLayout& layout,
                                 const double azimuthDegrees,
                                 const double height) {
    std::vector<std::size_t> bed;
    std::vector<std::size_t> upper;
    for (std::size_t index = 0; index < layout.speakers.size(); ++index) {
        if (Lowercase(layout.speakers[index].name) == L"lfe") continue;
        if (layout.speakers[index].elevationDegrees >= 25.0) {
            upper.push_back(index);
        } else {
            bed.push_back(index);
        }
    }
    if (bed.empty() && upper.empty()) throw std::runtime_error("Layout has no pannable speakers");

    const double normalizedHeight = std::clamp(height, 0.0, 1.0);
    double bedScale = upper.empty() ? 1.0 : std::cos(normalizedHeight * kPi / 2.0);
    double upperScale = bed.empty() ? 1.0 : std::sin(normalizedHeight * kPi / 2.0);
    std::vector<double> gains(layout.speakers.size());
    PanRing(layout, std::move(bed), azimuthDegrees, bedScale, gains);
    PanRing(layout, std::move(upper), azimuthDegrees, upperScale, gains);
    return gains;
}

std::vector<double> PanMatObject(const SpeakerLayout& layout,
                                 const std::array<unsigned, 6>& fields) {
    const double x = std::clamp((static_cast<double>(fields[0]) - 31.0) / 31.0,
                                -1.0, 1.0);
    const double z = std::clamp((static_cast<double>(fields[1]) - 31.0) / 31.0,
                                -1.0, 1.0);
    const double height = std::clamp((static_cast<double>(fields[2]) - 32.0) / 30.0,
                                     0.0, 1.0);
    const double azimuth = std::abs(x) + std::abs(z) < 1.0e-6
                               ? 0.0
                               : std::atan2(x, -z) * 180.0 / kPi;
    return PanDirection(layout, azimuth, height);
}

} // namespace dolby
