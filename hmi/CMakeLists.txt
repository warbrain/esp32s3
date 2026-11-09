cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProjectPaths.cmake")
hmi_require_build_path("${CMAKE_BINARY_DIR}" "Build output" "${CMAKE_CURRENT_SOURCE_DIR}")
# Reject output overrides before targets inherit them; keep the existing layout.
hmi_require_build_outputs("${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompilerCache.cmake" NO_POLICY_SCOPE)
project(HMI)
hmi_require_build_outputs("${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(BUILD_SHARED_LIBS OFF)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(3rd_party/glfw EXCLUDE_FROM_ALL)

set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
add_subdirectory(3rd_party/yaml-cpp EXCLUDE_FROM_ALL)
set_target_properties(yaml-cpp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
add_subdirectory(3rd_party/spdlog EXCLUDE_FROM_ALL)
add_library(hmi_base STATIC src/base/config.cpp src/base/logger.cpp src/base/performance.cpp)
target_include_directories(hmi_base PUBLIC src)
target_link_libraries(hmi_base PUBLIC yaml-cpp::yaml-cpp PRIVATE spdlog::spdlog pdh)
target_compile_definitions(hmi_base PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(hmi_base PRIVATE /W4 /permissive- /utf-8)
endif()

find_package(Vulkan REQUIRED)

add_library(imgui STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC 3rd_party/imgui 3rd_party/imgui/backends)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

set(APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/camera/camera_devices.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/microphone_devices.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/app.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/services.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/pages/voice_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/pages/history_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/device_controls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shell.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/desktop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/capture_overlay.cpp
)

include(CheckLanguage)
option(HMI_ENABLE_CUDA "Discover CUDA for the native SDK and optional diagnostics" ON)
option(HMI_ENABLE_OPENMP "Enable OpenMP by default in the native SDK" ON)
if(HMI_ENABLE_CUDA)
  check_language(CUDA)
endif()
set(HAS_CUDA OFF)
if(HMI_ENABLE_CUDA AND CMAKE_CUDA_COMPILER)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  # CUDA_PATH may name a toolkit that has since been removed; some subprojects
  # still resolve cudart from it, so pin everything to the one CMake found.
  link_directories("${CUDAToolkit_LIBRARY_DIR}")
  set(HAS_CUDA ON)
else()
  message(STATUS "CUDA compiler not found; native SDK and diagnostics can use non-CUDA backends")
endif()

# Vendored libopus (BSD-3-Clause, 3rd_party/opus) - the ESP32 bridge link is
# Opus on the wire, so the HMI needs its own decoder/encoder.
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/opus" "${CMAKE_CURRENT_BINARY_DIR}/opus" EXCLUDE_FROM_ALL)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AudioSource.cmake")
hmi_add_audio_source_sdk()

add_executable(${PROJECT_NAME} WIN32 ${APP_SOURCES})
add_library(hmi_voice STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/voice_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/capture/audio_capture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/native_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/audio_preprocessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/hgtcrn_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/model_location.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/asr_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/tts_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/audio_render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input/hotkey.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/intent/rule.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/intent/rule_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/json_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/json_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/action/actions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/action/ui_input.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/audio_link.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/esp32_audio.cpp)
target_include_directories(hmi_voice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src PRIVATE ${HMI_AUDIOCPP_HEADER_DIR})
target_compile_definitions(hmi_voice PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(HMI_BUILD_AUDIOCPP_FROM_SOURCE)
  target_compile_definitions(hmi_voice PUBLIC HMI_AUDIOCPP_SDK_OPENMP_ENABLED=$<BOOL:${HMI_AUDIOCPP_SDK_OPENMP}>)
else()
  target_compile_definitions(hmi_voice PUBLIC HMI_AUDIOCPP_SDK_OPENMP_ENABLED=0)
endif()
target_link_libraries(hmi_voice PUBLIC hmi_base ole32 oleaut32 uuid user32 shell32 uiautomationcore PRIVATE opus ws2_32)
if(MSVC)
  target_compile_options(hmi_voice PRIVATE /W4 /permissive- /utf-8)
endif()
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(${PROJECT_NAME} PRIVATE hmi_voice imgui Vulkan::Vulkan)
target_include_directories(${PROJECT_NAME} PRIVATE ${HMI_AUDIOCPP_HEADER_DIR})

# Keep the matching SDK plugins isolated from DLLs left by earlier builds.
# Run even when no application source needs recompiling after an SDK sync.
add_custom_target(hmi_runtime
  COMMAND ${CMAKE_COMMAND}
    "-DSOURCE_DIR=${HMI_AUDIOCPP_RUNTIME_SOURCE}"
    "-DDESTINATION_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>/native"
    -DREQUIRE_MANIFEST=ON
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployRuntime.cmake"
  COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/config"
    "-DDESTINATION_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>/config" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployTemplates.cmake"
  COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/rules"
    "-DDESTINATION_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>/rules" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployTemplates.cmake"
  VERBATIM)
if(HMI_BUILD_AUDIOCPP_FROM_SOURCE)
  add_dependencies(hmi_runtime hmi_audio_sdk)
endif()
add_dependencies(${PROJECT_NAME} hmi_runtime)
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE mf mfplat mfreadwrite mfuuid ole32 propsys dwmapi gdi32)
  target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>)
endif()

enable_testing()
add_subdirectory(tests)

# Explicit opt-in diagnostic: uses real models, never plays audio or sends input.
add_executable(hmi_voice_probe EXCLUDE_FROM_ALL tools/voice_probe.cpp)
target_link_libraries(hmi_voice_probe PRIVATE hmi_voice psapi)
target_compile_definitions(hmi_voice_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(hmi_voice_probe PRIVATE /W4 /permissive- /utf-8)
endif()

# Official ggml operator support/performance diagnostics, explicitly opt-in.
add_executable(hmi_app_startup_probe EXCLUDE_FROM_ALL tools/app_startup_probe.cpp)
target_link_libraries(hmi_app_startup_probe PRIVATE hmi_voice)
target_compile_definitions(hmi_app_startup_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(hmi_app_startup_probe PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(hmi_cudnn_kernel_probe EXCLUDE_FROM_ALL tools/cudnn_kernel_probe.cpp)
target_include_directories(hmi_cudnn_kernel_probe PRIVATE src 3rd_party/native/source/external/ggml/include)
target_compile_definitions(hmi_cudnn_kernel_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(hmi_cudnn_kernel_probe PRIVATE /W4 /permissive- /utf-8)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GgmlKernelProbe.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AudioSdkTuning.cmake")
add_dependencies(hmi_voice_probe hmi_runtime)

# Full-segment dual-channel enhancement and VAD; no microphone or speaker use.
add_executable(hmi_audio_frontend_probe EXCLUDE_FROM_ALL tools/audio_frontend_probe.cpp)
target_link_libraries(hmi_audio_frontend_probe PRIVATE hmi_voice psapi)
target_compile_definitions(hmi_audio_frontend_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(hmi_audio_frontend_probe PRIVATE /W4 /permissive- /utf-8)
endif()
add_dependencies(hmi_audio_frontend_probe hmi_runtime)
if(HMI_BUILD_AUDIOCPP_FROM_SOURCE)
  # SDK graph verification stays separate from the host and its C ABI facade.
  add_executable(hmi_hgtcrn_reference_probe EXCLUDE_FROM_ALL
    "${HMI_AUDIOCPP_SOURCE_ROOT}/tools/diagnostics/hgtcrn_reference_probe.cpp")
  target_link_libraries(hmi_hgtcrn_reference_probe PRIVATE engine_runtime)
  target_compile_definitions(hmi_hgtcrn_reference_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  set_target_properties(hmi_hgtcrn_reference_probe PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/audio-sdk/bin/$<CONFIG>")
  if(MSVC)
    target_compile_options(hmi_hgtcrn_reference_probe PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_dependencies(hmi_audio_frontend_probe hmi_hgtcrn_reference_probe)
endif()
add_dependencies(hmi_app_startup_probe hmi_runtime)
add_dependencies(hmi_cudnn_kernel_probe hmi_runtime)

# Production TTS worker with a silent, immediate PCM sink and no ASR model.
add_executable(hmi_tts_recovery_probe EXCLUDE_FROM_ALL tools/tts_recovery_probe.cpp)
target_link_libraries(hmi_tts_recovery_probe PRIVATE hmi_voice)
target_compile_definitions(hmi_tts_recovery_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(hmi_tts_recovery_probe PRIVATE /W4 /permissive- /utf-8)
endif()
add_dependencies(hmi_tts_recovery_probe hmi_runtime)

# Single-request SDK sampler/language comparisons; never plays or captures audio.
add_executable(hmi_tts_quality_probe EXCLUDE_FROM_ALL tools/tts_quality_probe.cpp)
target_include_directories(hmi_tts_quality_probe PRIVATE ${HMI_AUDIOCPP_HEADER_DIR})
target_link_libraries(hmi_tts_quality_probe PRIVATE hmi_voice)
target_compile_definitions(hmi_tts_quality_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(hmi_tts_quality_probe PRIVATE /W4 /permissive- /utf-8)
endif()
add_dependencies(hmi_tts_quality_probe hmi_runtime)
