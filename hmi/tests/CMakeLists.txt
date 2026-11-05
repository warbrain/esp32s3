# Keep test programs and fixture DLLs out of the application runtime directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# No third-party test framework: tests/check.hpp provides HMI_TEST and CHECK.
add_executable(hmi_tests ${TEST_SOURCES})
target_include_directories(hmi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(hmi_tests PRIVATE ${HMI_AUDIOCPP_HEADER_DIR})
target_compile_definitions(hmi_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(hmi_tests PRIVATE hmi_voice)
if(MSVC)
  target_compile_options(hmi_tests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME hmi_tests COMMAND hmi_tests)

# Exercise the real capture window without a microphone or external input.
add_executable(capture_overlay_integration
  "${PROJECT_SOURCE_DIR}/tools/capture_overlay_probe.cpp"
  "${PROJECT_SOURCE_DIR}/src/platform/capture_overlay.cpp")
target_include_directories(capture_overlay_integration PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(capture_overlay_integration PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(capture_overlay_integration PRIVATE user32 gdi32)
if(MSVC)
  target_compile_options(capture_overlay_integration PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME capture_overlay_integration COMMAND capture_overlay_integration)
set_tests_properties(capture_overlay_integration PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77)

# Exercise real shortcut state transitions with isolated WinAPI substitutes.
add_executable(hotkey_fallback_tests hotkey_fallback.cpp)
target_include_directories(hotkey_fallback_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(hotkey_fallback_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(hotkey_fallback_tests PRIVATE user32)
if(MSVC)
  target_compile_options(hotkey_fallback_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME hotkey_fallback_tests COMMAND hotkey_fallback_tests)

# Real loader regression: an old dependency beside the executable must not win.
set(SDK_LOADER_ROOT "${CMAKE_BINARY_DIR}/sdk-loader/$<CONFIG>")
add_library(sdk_loader_legacy SHARED sdk_loader_dependency.cpp)
add_library(sdk_loader_current SHARED sdk_loader_dependency.cpp)
target_compile_definitions(sdk_loader_legacy PRIVATE HMI_TEST_SDK_VALUE=11)
target_compile_definitions(sdk_loader_current PRIVATE HMI_TEST_SDK_VALUE=22)
set_target_properties(sdk_loader_legacy PROPERTIES
  OUTPUT_NAME hmi_test_sdk_dependency
  RUNTIME_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}"
  ARCHIVE_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}/import/legacy")
set_target_properties(sdk_loader_current PROPERTIES
  OUTPUT_NAME hmi_test_sdk_dependency
  RUNTIME_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}/current"
  ARCHIVE_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}/import/current")
add_library(sdk_loader_fixture SHARED sdk_loader_fixture.cpp)
target_link_libraries(sdk_loader_fixture PRIVATE sdk_loader_current)
set_target_properties(sdk_loader_fixture PROPERTIES
  OUTPUT_NAME hmi_test_sdk_fixture
  RUNTIME_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}/current"
  ARCHIVE_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}/import/fixture")
add_library(sdk_loader_external_runtime SHARED sdk_loader_external_runtime.cpp)
set_target_properties(sdk_loader_external_runtime PROPERTIES
  OUTPUT_NAME hmi_test_external_runtime
  RUNTIME_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}/runtime-only"
  ARCHIVE_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}/import/external-runtime")
add_library(sdk_loader_path_fixture SHARED sdk_loader_path_fixture.cpp)
target_link_libraries(sdk_loader_path_fixture PRIVATE sdk_loader_external_runtime)
set_target_properties(sdk_loader_path_fixture PROPERTIES
  OUTPUT_NAME hmi_test_sdk_path_fixture
  RUNTIME_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}/current"
  ARCHIVE_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}/import/path-fixture")
add_executable(sdk_loader_tests sdk_loader_tests.cpp)
target_include_directories(sdk_loader_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(sdk_loader_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(sdk_loader_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SDK_LOADER_ROOT}")
add_dependencies(sdk_loader_tests sdk_loader_legacy sdk_loader_fixture sdk_loader_path_fixture)
if(MSVC)
  foreach(target sdk_loader_legacy sdk_loader_current sdk_loader_fixture sdk_loader_tests
      sdk_loader_external_runtime sdk_loader_path_fixture)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  endforeach()
endif()
add_test(NAME sdk_loader_tests COMMAND sdk_loader_tests)

add_library(sdk_lifetime_fixture SHARED sdk_lifetime_fixture.cpp)
add_dependencies(hmi_tests sdk_lifetime_fixture)
target_include_directories(sdk_lifetime_fixture PRIVATE ${HMI_AUDIOCPP_HEADER_DIR})
target_compile_definitions(sdk_lifetime_fixture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_library(sdk_frontend_missing_fixture SHARED sdk_lifetime_fixture.cpp)
target_include_directories(sdk_frontend_missing_fixture PRIVATE ${HMI_AUDIOCPP_HEADER_DIR})
target_compile_definitions(sdk_frontend_missing_fixture PRIVATE HMI_TEST_NO_AUDIO_FRONTEND=1 NOMINMAX WIN32_LEAN_AND_MEAN)
add_dependencies(hmi_tests sdk_frontend_missing_fixture)
add_executable(sdk_lifetime_tests sdk_lifetime_tests.cpp)
target_include_directories(sdk_lifetime_tests PRIVATE ${HMI_AUDIOCPP_HEADER_DIR})
target_link_libraries(sdk_lifetime_tests PRIVATE hmi_voice)
target_compile_definitions(sdk_lifetime_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_dependencies(sdk_lifetime_tests sdk_lifetime_fixture)
if(MSVC)
  foreach(target sdk_lifetime_fixture sdk_frontend_missing_fixture sdk_lifetime_tests)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  endforeach()
endif()
add_test(NAME sdk_lifetime_tests COMMAND sdk_lifetime_tests $<TARGET_FILE:sdk_lifetime_fixture>)

# Exercise actual SDK diagnostics without a model or GPU backend dependency.
add_executable(hmi_native_diagnostics_tests native_diagnostics_tests.cpp
  "${PROJECT_SOURCE_DIR}/3rd_party/native/source/src/framework/debug/trace.cpp")
target_include_directories(hmi_native_diagnostics_tests PRIVATE "${PROJECT_SOURCE_DIR}/src"
  "${PROJECT_SOURCE_DIR}/3rd_party/native/source/include"
  "${PROJECT_SOURCE_DIR}/3rd_party/native/source/external/ggml/include")
target_compile_definitions(hmi_native_diagnostics_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(hmi_native_diagnostics_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(hmi_native_diagnostics_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME hmi_native_diagnostics_tests COMMAND hmi_native_diagnostics_tests)
set_tests_properties(hmi_native_diagnostics_tests PROPERTIES TIMEOUT 20)

# Public TTS interface tests with a deterministic SDK and no speaker device.
add_library(tts_fixture SHARED tts_fixture.cpp)
target_include_directories(tts_fixture PRIVATE ${HMI_AUDIOCPP_HEADER_DIR})
target_compile_definitions(tts_fixture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_executable(tts_segment_tests tts_segment_tests.cpp)
target_link_libraries(tts_segment_tests PRIVATE hmi_voice)
target_compile_definitions(tts_segment_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_dependencies(tts_segment_tests tts_fixture)
if(MSVC)
  foreach(target tts_fixture tts_segment_tests)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  endforeach()
endif()
add_test(NAME tts_segment_tests COMMAND tts_segment_tests $<TARGET_FILE:tts_fixture>)
set_tests_properties(tts_segment_tests PROPERTIES TIMEOUT 45)

add_executable(runtime_review_input runtime_review_input.cpp)
target_include_directories(runtime_review_input PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(runtime_review_input PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(runtime_review_input PRIVATE ole32 oleaut32 uiautomationcore user32 uuid)
if(MSVC)
  target_compile_options(runtime_review_input PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME runtime_review_input COMMAND runtime_review_input)

add_test(NAME runtime_deployment COMMAND ${CMAKE_COMMAND}
  "-DPROJECT_ROOT=${PROJECT_SOURCE_DIR}"
  "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/runtime-deployment"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/runtime_deployment.cmake")

add_test(NAME cudnn_discovery COMMAND ${CMAKE_COMMAND}
  "-DTEST_ROOT=${CMAKE_BINARY_DIR}/cudnn-fixtures"
  -P "${PROJECT_SOURCE_DIR}/tests/cudnn_discovery.cmake")

set(SDK_PREFLIGHT_ROOT "${CMAKE_BINARY_DIR}/sdk-preflight/$<CONFIG>")
add_library(sdk_mismatch_good SHARED sdk_mismatch_dependency.cpp)
add_library(sdk_mismatch_bad SHARED sdk_mismatch_dependency.cpp)
target_compile_definitions(sdk_mismatch_good PRIVATE HMI_TEST_NEW_SDK=1)
target_compile_definitions(sdk_mismatch_bad PRIVATE HMI_TEST_NEW_SDK=0)
set_target_properties(sdk_mismatch_good PROPERTIES OUTPUT_NAME ggml-test-dependency
  RUNTIME_OUTPUT_DIRECTORY "${SDK_PREFLIGHT_ROOT}/good" ARCHIVE_OUTPUT_DIRECTORY "${SDK_PREFLIGHT_ROOT}/import/good")
set_target_properties(sdk_mismatch_bad PROPERTIES OUTPUT_NAME ggml-test-dependency
  RUNTIME_OUTPUT_DIRECTORY "${SDK_PREFLIGHT_ROOT}/bad" ARCHIVE_OUTPUT_DIRECTORY "${SDK_PREFLIGHT_ROOT}/import/bad")
add_library(sdk_mismatch_fixture SHARED sdk_mismatch_fixture.cpp)
target_link_libraries(sdk_mismatch_fixture PRIVATE sdk_mismatch_good)
set_target_properties(sdk_mismatch_fixture PROPERTIES OUTPUT_NAME hmi_test_sdk_preflight
  RUNTIME_OUTPUT_DIRECTORY "${SDK_PREFLIGHT_ROOT}/good" ARCHIVE_OUTPUT_DIRECTORY "${SDK_PREFLIGHT_ROOT}/import/fixture")
add_custom_command(TARGET sdk_mismatch_fixture POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory "${SDK_PREFLIGHT_ROOT}/bad"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:sdk_mismatch_fixture>" "${SDK_PREFLIGHT_ROOT}/bad" VERBATIM)
add_executable(sdk_preflight_tests sdk_preflight_tests.cpp)
target_include_directories(sdk_preflight_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(sdk_preflight_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(sdk_preflight_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SDK_PREFLIGHT_ROOT}")
add_dependencies(sdk_preflight_tests sdk_mismatch_good sdk_mismatch_bad sdk_mismatch_fixture)
if(MSVC)
  foreach(target sdk_mismatch_good sdk_mismatch_bad sdk_mismatch_fixture sdk_preflight_tests)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  endforeach()
endif()
add_test(NAME sdk_preflight_tests COMMAND sdk_preflight_tests)

add_test(NAME deploy_templates COMMAND ${CMAKE_COMMAND}
  "-DTEST_ROOT=${CMAKE_BINARY_DIR}/template-fixtures"
  -P "${PROJECT_SOURCE_DIR}/tests/deploy_templates.cmake")
add_test(NAME source_sdk_staging COMMAND ${CMAKE_COMMAND}
  "-DTEST_ROOT=${CMAKE_BINARY_DIR}/sdk-staging-fixtures"
  -P "${PROJECT_SOURCE_DIR}/tests/source_sdk_staging.cmake")

add_test(NAME project_paths COMMAND ${CMAKE_COMMAND}
  "-DTEST_ROOT=${CMAKE_BINARY_DIR}/tests/project-paths"
  -P "${PROJECT_SOURCE_DIR}/tests/project_paths.cmake")

if(HMI_USE_CCACHE AND CMAKE_GENERATOR MATCHES "Ninja")
  add_test(NAME compiler_cache COMMAND ${CMAKE_COMMAND}
    "-DTEST_ROOT=${CMAKE_BINARY_DIR}/ccache-fixtures"
    "-DCCACHE_EXECUTABLE=${HMI_CCACHE_EXECUTABLE}"
    "-DTEST_CUDA=${HAS_CUDA}"
    "-DTEST_CUDA_ARCHITECTURES=${CMAKE_CUDA_ARCHITECTURES}"
    -P "${PROJECT_SOURCE_DIR}/tests/compiler_cache.cmake")
endif()

# Keep temporary files from C++ tests and their child processes in this build.
set(_hmi_test_tmp "${CMAKE_CURRENT_BINARY_DIR}/tmp")
hmi_require_project_path("${_hmi_test_tmp}" "Test temporary directory")
file(MAKE_DIRECTORY "${_hmi_test_tmp}")
foreach(_hmi_temp_target hmi_tests sdk_lifetime_tests tts_segment_tests hmi_native_diagnostics_tests)
  target_compile_definitions(${_hmi_temp_target} PRIVATE HMI_TEST_TMP_DIR=L"${_hmi_test_tmp}")
endforeach()
get_property(_hmi_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_hmi_tests} PROPERTIES
  ENVIRONMENT "TEMP=${_hmi_test_tmp};TMP=${_hmi_test_tmp}")

# Real OLE coverage has its own noninteractive station and private clipboard.
add_executable(ole_clipboard_integration ole_clipboard_integration.cpp)
target_include_directories(ole_clipboard_integration PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(ole_clipboard_integration PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(ole_clipboard_integration PRIVATE ole32 oleaut32 user32 gdi32 uuid)
if(MSVC)
  target_compile_options(ole_clipboard_integration PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME ole_clipboard_integration COMMAND ole_clipboard_integration)
set_tests_properties(ole_clipboard_integration PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77
  ENVIRONMENT "TEMP=${_hmi_test_tmp};TMP=${_hmi_test_tmp}")
