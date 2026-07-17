#include "audio_platform.h"
#include "commands.h"
#include "mat_capture_client.h"
#include "multi_endpoint_renderer.h"
#include "speaker_layout.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace dolby {

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) throw std::runtime_error("Could not encode endpoint name as UTF-8");
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                            result.data(), bytes, nullptr, nullptr) != bytes) {
        throw std::runtime_error("Could not encode endpoint name as UTF-8");
    }
    return result;
}

void PrintUsage() {
    std::wcout
        << L"dolby-probe list\n"
        << L"dolby-probe list-endpoints\n"
        << L"dolby-probe set-default [endpoint-filter]\n"
        << L"dolby-probe render-test [seconds] [endpoint-filter] [pcm|mat20|mat21]\n"
        << L"dolby-probe replay-iec61937 input.wav [endpoint-filter] [repeat]\n"
        << L"dolby-probe spatial-test [seconds] [endpoint-filter] "
           L"[bed|height|712|714|dynamic|dynamic-<position>|silence|impulse-<channel>]\n"
        << L"dolby-probe capture [seconds] [endpoint-filter] [output.wav]\n"
        << L"dolby-probe capture-process [seconds] [pid] [output.wav]\n"
        << L"dolby-probe list-audio-decoders [filter] [inspect]\n"
        << L"dolby-probe probe-winrt-decoder [runtime-class]\n"
        << L"dolby-probe probe-spatial-metadata [endpoint-filter]\n"
        << L"dolby-probe probe-dtsx-license [codec-name]\n"
        << L"dolby-probe probe-dtsx-field-of-use\n"
        << L"dolby-probe probe-dtsx-decode input.wav [max-bursts] [spatial|pcm] "
           L"[output.wav]\n"
        << L"dolby-probe probe-media-types input-media\n"
        << L"dolby-probe capture-mat-ring [seconds] [output.wav] [poll-ms]\n"
        << L"dolby-probe capture-iec61937-ring [seconds] [output.wav] [poll-ms]\n"
        << L"dolby-probe analyze input.wav\n"
        << L"dolby-probe analyze-iec61937 input.wav\n"
        << L"dolby-probe extract-dtshd input.wav output.dts\n"
        << L"dolby-probe analyze-dtsx-exss input.dts\n"
        << L"dolby-probe analyze-mat input.wav\n"
        << L"dolby-probe analyze-mat-layout input.wav layout.ini\n"
        << L"dolby-probe compare-mat-positions origin.wav left.wav right.wav above.wav "
           L"front.wav behind.wav\n"
        << L"dolby-probe analyze-mat-positions input.wav\n"
        << L"dolby-probe extract-mat-712 input.wav output.wav\n\n"
        << L"dolby-probe play-712 input.wav [rear-filter] [height-filter] [gain] [repeat]\n\n"
        << L"dolby-probe live-712 [seconds] [rear-filter] [height-filter] [gain] [prebuffer-ms]\n\n"
        << L"dolby-probe test-layout [seconds] [layout.ini] [gain] [safe|balanced|low]\n"
        << L"dolby-probe test-speaker [seconds] [layout.ini] [speaker] [gain] "
           L"[safe|balanced|low]\n"
        << L"dolby-probe live-layout [seconds] [layout.ini] [gain] [prebuffer-ms] "
           L"[safe|balanced|low]\n\n"
        << L"dolby-probe live-dtsx-layout [seconds] [layout.ini] [gain] [prebuffer-ms] "
           L"[safe|balanced|low]\n\n"
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

        if (command == L"list-endpoints") {
            for (const auto& endpoint : EnumerateRenderEndpoints()) {
                std::cout << Utf8(endpoint.name) << '\t' << Utf8(endpoint.id) << '\n';
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

        if (command == L"replay-iec61937") {
            if (argc < 3) {
                throw std::runtime_error("replay-iec61937 requires an input WAV path");
            }
            const std::wstring filter = argc >= 4 ? argv[3] : L"SinkDescription Sample";
            const std::uint64_t repeatCount = argc >= 5 ? std::stoull(argv[4]) : 1;
            if (repeatCount == 0 || repeatCount > 1'000) {
                throw std::runtime_error("IEC 61937 repeat count must be between 1 and 1000");
            }
            ReplayIec61937Wave(argv[2], filter, repeatCount);
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

        if (command == L"test-layout") {
            const double seconds = argc >= 3 ? std::stod(argv[2]) : 30.0;
            const std::filesystem::path layoutPath =
                argc >= 4 ? argv[3] : L"configs\\realtek-c1u-714.ini";
            const double gain = argc >= 5 ? std::stod(argv[4]) : 0.0;
            const RendererLatencyMode latencyMode = argc >= 6
                ? ParseRendererLatencyMode(argv[5])
                : RendererLatencyMode::Safe;
            if (seconds <= 0.0 || seconds > 3'600.0 || gain < 0.0 || gain > 1.0) {
                throw std::runtime_error(
                    "test-layout requires 0 < seconds <= 3600 and 0 <= gain <= 1");
            }
            TestSpeakerLayout(seconds, LoadSpeakerLayout(layoutPath), gain, {}, latencyMode);
            return 0;
        }

        if (command == L"test-speaker") {
            if (argc < 5) {
                throw std::runtime_error(
                    "test-speaker requires seconds, layout INI and speaker name");
            }
            const double seconds = std::stod(argv[2]);
            const std::filesystem::path layoutPath = argv[3];
            const std::wstring speakerName = argv[4];
            const double gain = argc >= 6 ? std::stod(argv[5]) : 0.10;
            const RendererLatencyMode latencyMode = argc >= 7
                ? ParseRendererLatencyMode(argv[6])
                : RendererLatencyMode::Safe;
            if (seconds <= 0.0 || seconds > 60.0 || gain < 0.0 || gain > 1.0) {
                throw std::runtime_error(
                    "test-speaker requires 0 < seconds <= 60 and 0 <= gain <= 1");
            }
            TestSpeakerLayout(
                seconds, LoadSpeakerLayout(layoutPath), gain, speakerName, latencyMode);
            return 0;
        }

        if (command == L"live-layout") {
            const double seconds = argc >= 3 ? std::stod(argv[2]) : 60.0;
            const std::filesystem::path layoutPath =
                argc >= 4 ? argv[3] : L"configs\\realtek-c1u-714.ini";
            const double gain = argc >= 5 ? std::stod(argv[4]) : 0.25;
            const DWORD prebufferMilliseconds =
                argc >= 6 ? static_cast<DWORD>(std::stoul(argv[5])) : 40;
            const RendererLatencyMode latencyMode = argc >= 7
                ? ParseRendererLatencyMode(argv[6])
                : RendererLatencyMode::Balanced;
            if (seconds <= 0.0 || seconds > 3'600.0 || gain < 0.0 || gain > 1.0 ||
                prebufferMilliseconds < 20 || prebufferMilliseconds > 500) {
                throw std::runtime_error(
                    "live-layout requires 0 < seconds <= 3600, 0 <= gain <= 1 and "
                    "20 <= prebuffer-ms <= 500");
            }
            PlayLiveMatLayout(
                seconds, layoutPath, gain, prebufferMilliseconds, latencyMode);
            return 0;
        }

        if (command == L"live-dtsx-layout") {
            const double seconds = argc >= 3 ? std::stod(argv[2]) : 60.0;
            const std::filesystem::path layoutPath =
                argc >= 4 ? argv[3] : L"configs\\realtek-c1u-714.ini";
            const double gain = argc >= 5 ? std::stod(argv[4]) : 0.25;
            const DWORD prebufferMilliseconds =
                argc >= 6 ? static_cast<DWORD>(std::stoul(argv[5])) : 40;
            const RendererLatencyMode latencyMode = argc >= 7
                ? ParseRendererLatencyMode(argv[6])
                : RendererLatencyMode::Balanced;
            if (seconds <= 0.0 || seconds > 3'600.0 || gain < 0.0 || gain > 1.0 ||
                prebufferMilliseconds < 20 || prebufferMilliseconds > 500) {
                throw std::runtime_error(
                    "live-dtsx-layout requires 0 < seconds <= 3600, 0 <= gain <= 1 and "
                    "20 <= prebuffer-ms <= 500");
            }
            PlayLiveDtsXLayout(
                seconds, layoutPath, gain, prebufferMilliseconds, latencyMode);
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

        if (command == L"list-audio-decoders") {
            const std::wstring filter = argc >= 3 ? argv[2] : L"";
            const bool inspectTypes = argc >= 4 && Lowercase(argv[3]) == L"inspect";
            ListAudioDecoders(filter, inspectTypes);
            return 0;
        }

        if (command == L"probe-winrt-decoder") {
            const std::wstring runtimeClass =
                argc >= 3 ? argv[2] : L"DTSXDecoder.DTSXDecoder";
            ProbeWinRtDecoder(runtimeClass);
            return 0;
        }

        if (command == L"probe-spatial-metadata") {
            const std::wstring filter = argc >= 3 ? argv[2] : L"SinkDescription Sample";
            ProbeSpatialMetadata(filter);
            return 0;
        }

        if (command == L"probe-dtsx-license") {
            const std::wstring codecName = argc >= 3 ? argv[2] : L"DTSXDecoder";
            ProbeDtsXLicense(codecName);
            return 0;
        }

        if (command == L"probe-dtsx-field-of-use") {
            ProbeDtsXFieldOfUse();
            return 0;
        }

        if (command == L"probe-dtsx-decode") {
            if (argc < 3) {
                throw std::runtime_error("probe-dtsx-decode requires an IEC 61937 WAV path");
            }
            const std::size_t maxBursts = argc >= 4 ? std::stoull(argv[3]) : 16;
            if (maxBursts == 0 || maxBursts > 10'000) {
                throw std::runtime_error("max-bursts must be between 1 and 10000");
            }
            const std::wstring mode = argc >= 5 ? Lowercase(argv[4]) : L"spatial";
            if (mode != L"spatial" && mode != L"pcm") {
                throw std::runtime_error("DTS:X output mode must be spatial or pcm");
            }
            const std::filesystem::path outputPath = argc >= 6 ? argv[5] : L"";
            if (mode == L"spatial" && !outputPath.empty()) {
                throw std::runtime_error("An output WAV path is valid only in pcm mode");
            }
            ProbeDtsXDecode(argv[2], maxBursts,
                            mode == L"spatial" ? DtsXDecodeOutput::SpatialObjects
                                               : DtsXDecodeOutput::Pcm71,
                            outputPath);
            return 0;
        }

        if (command == L"probe-media-types") {
            if (argc < 3) throw std::runtime_error("probe-media-types requires an input path");
            ProbeMediaTypes(argv[2]);
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

        if (command == L"capture-iec61937-ring") {
            const double seconds = argc >= 3 ? std::stod(argv[2]) : 10.0;
            const std::filesystem::path output = argc >= 4 ? argv[3] : L"iec61937-ring-capture.wav";
            const DWORD pollMilliseconds = argc >= 5 ? static_cast<DWORD>(std::stoul(argv[4])) : 2;
            if (seconds <= 0.0 || seconds > 600.0 || pollMilliseconds == 0 ||
                pollMilliseconds > 1000) {
                throw std::runtime_error(
                    "Ring capture requires 0 < seconds <= 600 and 0 < poll-ms <= 1000");
            }
            CaptureIec61937Ring(seconds, output, pollMilliseconds);
            return 0;
        }

        if (command == L"analyze") {
            if (argc < 3) throw std::runtime_error("analyze requires an input WAV path");
            AnalyzeFloatWave(argv[2]);
            return 0;
        }

        if (command == L"analyze-iec61937") {
            if (argc < 3) throw std::runtime_error("analyze-iec61937 requires an input WAV path");
            AnalyzeIec61937Wave(argv[2]);
            return 0;
        }

        if (command == L"extract-dtshd") {
            if (argc < 4) throw std::runtime_error("extract-dtshd requires input and output paths");
            ExtractDtsHdWave(argv[2], argv[3]);
            return 0;
        }

        if (command == L"analyze-dtsx-exss") {
            if (argc < 3) {
                throw std::runtime_error("analyze-dtsx-exss requires an input DTS path");
            }
            AnalyzeDtsXExss(argv[2]);
            return 0;
        }

        if (command == L"analyze-mat") {
            if (argc < 3) throw std::runtime_error("analyze-mat requires an input WAV path");
            AnalyzeMatWave(argv[2]);
            return 0;
        }

        if (command == L"analyze-mat-layout") {
            if (argc != 4) {
                throw std::runtime_error(
                    "analyze-mat-layout requires an input WAV and a layout INI");
            }
            AnalyzeMatLayout(argv[2], argv[3]);
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
