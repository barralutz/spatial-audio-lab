#include "audio_platform.h"
#include "commands.h"
#include "mat_capture_client.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace dolby {

void PrintUsage() {
    std::wcout
        << L"dolby-probe list\n"
        << L"dolby-probe set-default [endpoint-filter]\n"
        << L"dolby-probe render-test [seconds] [endpoint-filter] [pcm|mat20|mat21]\n"
        << L"dolby-probe spatial-test [seconds] [endpoint-filter] "
           L"[bed|height|712|dynamic|dynamic-<position>|silence|impulse-<channel>]\n"
        << L"dolby-probe capture [seconds] [endpoint-filter] [output.wav]\n"
        << L"dolby-probe capture-process [seconds] [pid] [output.wav]\n"
        << L"dolby-probe capture-mat-ring [seconds] [output.wav] [poll-ms]\n"
        << L"dolby-probe analyze input.wav\n"
        << L"dolby-probe analyze-mat input.wav\n"
        << L"dolby-probe compare-mat-positions origin.wav left.wav right.wav above.wav "
           L"front.wav behind.wav\n"
        << L"dolby-probe analyze-mat-positions input.wav\n"
        << L"dolby-probe extract-mat-712 input.wav output.wav\n\n"
        << L"dolby-probe play-712 input.wav [rear-filter] [height-filter] [gain] [repeat]\n\n"
        << L"dolby-probe live-712 [seconds] [rear-filter] [height-filter] [gain] [prebuffer-ms]\n\n"
        << L"An empty endpoint filter selects the default render endpoint.\n";
}
} // namespace dolby

int wmain(const int argc, wchar_t** argv) {
    using namespace dolby;
    try {
        ComApartment apartment;
        if (argc < 2) {
            PrintUsage();
            return 2;
        }

        const std::wstring command = Lowercase(argv[1]);
        if (command == L"list") {
            for (const auto& endpoint : EnumerateRenderEndpoints()) {
                PrintEndpoint(endpoint);
            }
            return 0;
        }

        if (command == L"set-default") {
            const std::wstring filter = argc >= 3 ? argv[2] : L"SinkDescription Sample";
            SetDefaultEndpoint(filter);
            return 0;
        }

        if (command == L"capture") {
            const double seconds = argc >= 3 ? std::stod(argv[2]) : 10.0;
            std::wstring filter = argc >= 4 ? argv[3] : L"";
            if (filter == L"-" || Lowercase(filter) == L"default") filter.clear();
            const std::filesystem::path output = argc >= 5 ? argv[4] : L"capture.wav";
            if (seconds <= 0.0 || seconds > 3600.0) {
                throw std::runtime_error("Capture duration must be between 0 and 3600 seconds");
            }
            CaptureLoopback(seconds, filter, output);
            return 0;
        }

        if (command == L"render-test") {
            const double seconds = argc >= 3 ? std::stod(argv[2]) : 2.0;
            std::wstring filter = argc >= 4 ? argv[3] : L"SinkDescription Sample";
            if (filter == L"-" || Lowercase(filter) == L"default") filter.clear();
            const std::wstring mode = argc >= 5 ? Lowercase(argv[4]) : L"mat20";
            if (seconds <= 0.0 || seconds > 60.0) {
                throw std::runtime_error("Render duration must be between 0 and 60 seconds");
            }
            RenderTransportTest(seconds, filter, mode);
            return 0;
        }

        if (command == L"spatial-test") {
            const double seconds = argc >= 3 ? std::stod(argv[2]) : 2.0;
            std::wstring filter = argc >= 4 ? argv[3] : L"SinkDescription Sample";
            if (filter == L"-" || Lowercase(filter) == L"default") filter.clear();
            const std::wstring mode = argc >= 5 ? Lowercase(argv[4]) : L"712";
            if (seconds <= 0.0 || seconds > 60.0) {
                throw std::runtime_error("Spatial duration must be between 0 and 60 seconds");
            }
            SpatialSignalTest(seconds, filter, mode);
            return 0;
        }

        if (command == L"extract-mat-712") {
            if (argc < 4) throw std::runtime_error("Input and output WAV paths are required");
            ExtractMat712Wave(argv[2], argv[3]);
            return 0;
        }

        if (command == L"play-712") {
            if (argc < 3) throw std::runtime_error("play-712 requires an input WAV path");
            const std::wstring rearFilter = argc >= 4 ? argv[3] : L"Altavoces (Realtek(R) Audio)";
            const std::wstring heightFilter = argc >= 5 ? argv[4] : L"2nd output";
            const double gain = argc >= 6 ? std::stod(argv[5]) : 0.25;
            const std::uint64_t repeatCount = argc >= 7 ? std::stoull(argv[6]) : 1;
            PlayAnalog712(argv[2], rearFilter, heightFilter, gain, repeatCount);
            return 0;
        }

        if (command == L"live-712") {
            const double seconds = argc >= 3 ? std::stod(argv[2]) : 60.0;
            const std::wstring rearFilter = argc >= 4 ? argv[3] : L"Altavoces (Realtek(R) Audio)";
            const std::wstring heightFilter = argc >= 5 ? argv[4] : L"2nd output";
            const double gain = argc >= 6 ? std::stod(argv[5]) : 0.25;
            const DWORD prebufferMilliseconds =
                argc >= 7 ? static_cast<DWORD>(std::stoul(argv[6])) : 80;
            if (seconds <= 0.0 || seconds > 3'600.0 ||
                prebufferMilliseconds < 20 || prebufferMilliseconds > 500) {
                throw std::runtime_error(
                    "live-712 requires 0 < seconds <= 3600 and 20 <= prebuffer-ms <= 500");
            }
            PlayLiveMat712(seconds, rearFilter, heightFilter, gain, prebufferMilliseconds);
            return 0;
        }

        if (command == L"capture-process") {
            if (argc < 4) {
                throw std::runtime_error("capture-process requires seconds and a process ID");
            }
            const double seconds = std::stod(argv[2]);
            const auto processId = static_cast<DWORD>(std::stoul(argv[3]));
            const std::filesystem::path output = argc >= 5 ? argv[4] : L"process-capture.wav";
            if (seconds <= 0.0 || seconds > 3600.0 || processId == 0) {
                throw std::runtime_error("Invalid capture duration or process ID");
            }
            CaptureProcessLoopback(seconds, processId, output);
            return 0;
        }

        if (command == L"capture-mat-ring") {
            const double seconds = argc >= 3 ? std::stod(argv[2]) : 10.0;
            const std::filesystem::path output = argc >= 4 ? argv[3] : L"mat-ring-capture.wav";
            const DWORD pollMilliseconds = argc >= 5 ? static_cast<DWORD>(std::stoul(argv[4])) : 2;
            if (seconds <= 0.0 || seconds > 600.0 || pollMilliseconds == 0 ||
                pollMilliseconds > 1000) {
                throw std::runtime_error(
                    "Ring capture requires 0 < seconds <= 600 and 0 < poll-ms <= 1000");
            }
            CaptureMatRing(seconds, output, pollMilliseconds);
            return 0;
        }

        if (command == L"analyze") {
            if (argc < 3) throw std::runtime_error("analyze requires an input WAV path");
            AnalyzeFloatWave(argv[2]);
            return 0;
        }

        if (command == L"analyze-mat") {
            if (argc < 3) throw std::runtime_error("analyze-mat requires an input WAV path");
            AnalyzeMatWave(argv[2]);
            return 0;
        }

        if (command == L"compare-mat-positions") {
            if (argc != 8) {
                throw std::runtime_error(
                    "compare-mat-positions requires origin, left, right, above, front and behind WAV paths");
            }
            CompareMatPositionFixtures({argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]});
            return 0;
        }

        if (command == L"analyze-mat-positions") {
            if (argc != 3) {
                throw std::runtime_error("analyze-mat-positions requires an input WAV path");
            }
            AnalyzeMatPositionTimeline(argv[2]);
            return 0;
        }

        PrintUsage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
