cmake_minimum_required(VERSION 3.24)

project(SpatialAudioLab LANGUAGES CXX)

add_executable(SpatialAudioLab.CLI
  src/audio_platform.cpp
  src/bridge_meter.cpp
  src/capture_commands.cpp
  src/dolby_probe.cpp
  src/dtsx_analysis.cpp
  src/media_foundation_probe.cpp
  src/mat_analysis.cpp
  src/mat_capture_client.cpp
  src/mat_format.cpp
  src/mat_pipeline.cpp
  src/multi_endpoint_renderer.cpp
  src/pcm_pipeline.cpp
  src/speaker_layout.cpp
  src/spatial_audio_sample.cpp
  src/wave_io.cpp
)
target_compile_features(SpatialAudioLab.CLI PRIVATE cxx_std_20)
target_compile_definitions(SpatialAudioLab.CLI PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(SpatialAudioLab.CLI PRIVATE ole32 runtimeobject windowsapp uuid avrt propsys mmdevapi mf mfplat mfreadwrite mfuuid)

if(MSVC)
  target_compile_options(SpatialAudioLab.CLI PRIVATE /W4 /permissive- /EHsc)
endif()
