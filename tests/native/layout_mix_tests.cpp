#include "layout_mix.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using dolby::Canonical714Mix;
using dolby::SpeakerDefinition;
using dolby::SpeakerLayout;

constexpr double kTolerance = 1.0e-9;
constexpr double kEqualPower = 0.70710678118654752440;

void Require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void RequireNear(const double actual, const double expected,
                 const std::string& message) {
    if (std::abs(actual - expected) > kTolerance) {
        throw std::runtime_error(message + ": expected " + std::to_string(expected) +
                                 ", got " + std::to_string(actual));
    }
}

SpeakerDefinition Speaker(const wchar_t* name, const double azimuth,
                          const double elevation) {
    return {name, azimuth, elevation, 0.0};
}

SpeakerLayout Canonical714() {
    SpeakerLayout layout;
    layout.name = L"7.1.4";
    layout.speakers = {
        Speaker(L"FL", -30, 0), Speaker(L"FR", 30, 0),
        Speaker(L"FC", 0, 0), Speaker(L"LFE", 0, 0),
        Speaker(L"BL", -150, 0), Speaker(L"BR", 150, 0),
        Speaker(L"SL", -90, 0), Speaker(L"SR", 90, 0),
        Speaker(L"TFL", -45, 45), Speaker(L"TFR", 45, 45),
        Speaker(L"TBL", -135, 45), Speaker(L"TBR", 135, 45),
    };
    return layout;
}

SpeakerLayout TopMiddle512() {
    SpeakerLayout layout;
    layout.name = L"5.1.2 top middle";
    layout.speakers = {
        Speaker(L"FL", -30, 0), Speaker(L"FR", 30, 0),
        Speaker(L"FC", 0, 0), Speaker(L"LFE", 0, 0),
        Speaker(L"SL", -90, 0), Speaker(L"SR", 90, 0),
        Speaker(L"TML", -90, 90), Speaker(L"TMR", 90, 90),
    };
    return layout;
}

SpeakerLayout Stereo() {
    SpeakerLayout layout;
    layout.name = L"Stereo";
    layout.speakers = {
        Speaker(L"FL", -30, 0), Speaker(L"FR", 30, 0),
    };
    return layout;
}

void IdentityMatrixPreservesCanonical714() {
    const Canonical714Mix mix = dolby::BuildCanonical714Mix(Canonical714());
    Require(mix.size() == 12, "Identity matrix must have 12 source rows");
    for (std::size_t source = 0; source < mix.size(); ++source) {
        Require(mix[source].size() == 12, "Identity row must have 12 destinations");
        for (std::size_t destination = 0; destination < mix[source].size(); ++destination) {
            RequireNear(mix[source][destination], source == destination ? 1.0 : 0.0,
                        "7.1.4 identity coefficient mismatch");
        }
    }
}

void TopMiddleFoldsHeightPairsAtEqualPower() {
    const Canonical714Mix mix = dolby::BuildCanonical714Mix(TopMiddle512());
    constexpr std::size_t tml = 6;
    constexpr std::size_t tmr = 7;
    RequireNear(mix[8][tml], kEqualPower, "TFL must fold into TML");
    RequireNear(mix[10][tml], kEqualPower, "TBL must fold into TML");
    RequireNear(mix[9][tmr], kEqualPower, "TFR must fold into TMR");
    RequireNear(mix[11][tmr], kEqualPower, "TBR must fold into TMR");
}

void StereoFoldsCenterAndSurroundByDirection() {
    const Canonical714Mix mix = dolby::BuildCanonical714Mix(Stereo());
    RequireNear(mix[2][0], kEqualPower, "Center must feed stereo left equally");
    RequireNear(mix[2][1], kEqualPower, "Center must feed stereo right equally");
    Require(mix[4][0] > mix[4][1], "Back-left must favor the left speaker");
    Require(mix[7][1] > mix[7][0], "Side-right must favor the right speaker");
}

void MissingLfeProducesSilentRow() {
    const Canonical714Mix mix = dolby::BuildCanonical714Mix(Stereo());
    Require(std::all_of(mix[3].begin(), mix[3].end(),
                        [](const double gain) { return gain == 0.0; }),
            "Missing LFE must produce a silent row");
}

void Version2ProfileLoadsWithoutLfe() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "SpatialAudioLab-native-stereo.ini";
    {
        std::ofstream profile(path, std::ios::binary | std::ios::trunc);
        profile <<
            "[profile]\n"
            "version=2\n"
            "id=11111111-2222-3333-4444-555555555555\n"
            "name=Native stereo\n"
            "layout=2.0\n\n"
            "[layout]\n"
            "speakers=FL,FR\n"
            "outputs=main\n"
            "master=main\n\n"
            "[speaker.FL]\n"
            "azimuth=-30\n"
            "elevation=0\n"
            "trim_db=0\n\n"
            "[speaker.FR]\n"
            "azimuth=30\n"
            "elevation=0\n"
            "trim_db=0\n\n"
            "[output.main]\n"
            "endpoint_id=endpoint-1\n"
            "endpoint_name=Speakers\n"
            "expected_channels=2\n"
            "speakers=FL,FR\n"
            "delay_ms=0\n";
    }

    try {
        const SpeakerLayout layout = dolby::LoadSpeakerLayout(path);
        Require(layout.name == L"Native stereo", "Version 2 profile name was not loaded");
        Require(layout.speakers.size() == 2, "Version 2 stereo speaker count mismatch");
        Require(layout.outputs.front().endpointFilter == L"endpoint-1",
                "Version 2 endpoint ID was not selected");
    } catch (...) {
        std::filesystem::remove(path);
        throw;
    }
    std::filesystem::remove(path);
}

void EverySourceRowHasAtMostUnityPower() {
    for (const SpeakerLayout& layout : {Canonical714(), TopMiddle512(), Stereo()}) {
        const Canonical714Mix mix = dolby::BuildCanonical714Mix(layout);
        for (const auto& row : mix) {
            double power = 0.0;
            for (const double gain : row) power += gain * gain;
            Require(power <= 1.0 + kTolerance, "Mix row exceeds unity power");
        }
    }
}

void FrameMixingSaturatesIntegerAndFloatOutput() {
    Canonical714Mix mix(12, std::vector<double>(1, 0.0));
    mix[0][0] = 1.0;
    mix[1][0] = 1.0;
    std::array<std::int16_t, 12> integers{};
    integers[0] = 30'000;
    integers[1] = 30'000;
    std::array<float, 12> floats{};
    floats[0] = 0.75F;
    floats[1] = 0.75F;

    const auto integerOutput = dolby::MixCanonical714Frame(integers, mix);
    const auto floatOutput = dolby::MixCanonical714Frame(floats, mix);

    Require(integerOutput == std::vector<std::int16_t>{32'767},
            "Integer mix must saturate at INT16_MAX");
    Require(floatOutput == std::vector<float>{1.0F},
            "Float mix must saturate at unity");
}

void FrameMixingCanReuseCallerOwnedBuffer() {
    Canonical714Mix mix(12, std::vector<double>(2, 0.0));
    mix[0][0] = 1.0;
    mix[1][1] = 1.0;
    std::array<std::int16_t, 12> source{};
    source[0] = 123;
    source[1] = -456;
    std::array<std::int16_t, 2> output{};

    dolby::MixCanonical714Frame(source, mix, output);

    Require(output[0] == 123 && output[1] == -456,
            "Caller-owned frame buffer was not populated");
}

} // namespace

int main() {
    try {
        IdentityMatrixPreservesCanonical714();
        TopMiddleFoldsHeightPairsAtEqualPower();
        StereoFoldsCenterAndSurroundByDirection();
        MissingLfeProducesSilentRow();
        Version2ProfileLoadsWithoutLfe();
        EverySourceRowHasAtMostUnityPower();
        FrameMixingSaturatesIntegerAndFloatOutput();
        FrameMixingCanReuseCallerOwnedBuffer();
        std::cout << "SpatialAudioLab native layout mix tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
