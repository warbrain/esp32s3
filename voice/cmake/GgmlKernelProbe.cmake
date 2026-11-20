include_guard(GLOBAL)

set(_hmi_default_ggml_sdk "${PROJECT_SOURCE_DIR}/build/audio-sdk/package/Release")
if(DEFINED HMI_GGML_SDK_ROOT)
  file(TO_CMAKE_PATH "${HMI_GGML_SDK_ROOT}" _hmi_previous_sdk)
  cmake_path(ABSOLUTE_PATH _hmi_previous_sdk BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE _hmi_previous_sdk)
  string(TOLOWER "${_hmi_previous_sdk}" _hmi_previous_sdk)
  string(TOLOWER "${PROJECT_SOURCE_DIR}/3rd_party/audiocpp" _hmi_legacy_sdk)
  string(TOLOWER "${PROJECT_SOURCE_DIR}/3rd_party/native" _hmi_previous_native_sdk)
  if(_hmi_previous_sdk STREQUAL _hmi_legacy_sdk OR _hmi_previous_sdk STREQUAL _hmi_previous_native_sdk)
    set(HMI_GGML_SDK_ROOT "${_hmi_default_ggml_sdk}" CACHE PATH "Matching prebuilt native SDK used by the ggml diagnostics" FORCE)
    set(HMI_GGML_SDK_ROOT "${_hmi_default_ggml_sdk}")
  endif()
endif()
option(HMI_BUILD_GGML_KERNEL_PROBE "Build the official ggml kernel support/performance diagnostics" OFF)
if(NOT HMI_BUILD_GGML_KERNEL_PROBE)
  return()
endif()

if(NOT WIN32)
  message(FATAL_ERROR "The ggml kernel probe requires the matching Windows native SDK.")
endif()

set(HMI_GGML_SDK_ROOT "${_hmi_default_ggml_sdk}" CACHE PATH "Matching prebuilt native SDK used by the ggml diagnostics")
include("${CMAKE_CURRENT_LIST_DIR}/ProjectPaths.cmake")
hmi_require_build_path("${HMI_GGML_SDK_ROOT}" "ggml diagnostic SDK")
set(HMI_GGML_KERNEL_BACKEND "CPU" CACHE STRING "Exact ggml device name, such as CPU, CUDA0 or Vulkan0")
set_property(CACHE HMI_GGML_KERNEL_BACKEND PROPERTY STRINGS CPU CUDA0 Vulkan0)
set(HMI_GGML_KERNEL_OPS "SOFT_MAX" CACHE STRING "Comma-separated official ggml operation names")
set(HMI_GGML_SUPPORT_PARAMS "^type=f32," CACHE STRING "Official ggml support case parameter regex")
set(HMI_GGML_PERF_PARAMS [=[^type=f32,ne=\[64,64,20,1\],]=] CACHE STRING "Official ggml performance case regex; the default selects one small case")
set(HMI_GGML_KERNEL_TIMEOUT "120" CACHE STRING "Maximum seconds for each explicit ggml diagnostic run")

if(HMI_GGML_KERNEL_BACKEND STREQUAL "" OR HMI_GGML_KERNEL_OPS STREQUAL "")
  message(FATAL_ERROR "HMI_GGML_KERNEL_BACKEND and HMI_GGML_KERNEL_OPS must not be empty.")
endif()
if(HMI_GGML_KERNEL_OPS MATCHES ";")
  message(FATAL_ERROR "Use commas, not semicolons, in HMI_GGML_KERNEL_OPS.")
endif()
if(NOT HMI_GGML_KERNEL_TIMEOUT MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "HMI_GGML_KERNEL_TIMEOUT must be a positive integer.")
endif()
set(_hmi_ggml_root "${HMI_AUDIOCPP_SOURCE_ROOT}/external/ggml")
hmi_require_project_path("${_hmi_ggml_root}" "ggml diagnostic sources")
set(_hmi_ggml_ops_source "${_hmi_ggml_root}/tests/test-backend-ops.cpp")
foreach(_hmi_ggml_file IN ITEMS
    "${_hmi_ggml_ops_source}"
    "${_hmi_ggml_root}/include/ggml.h"
    "${_hmi_ggml_root}/include/ggml-alloc.h"
    "${_hmi_ggml_root}/include/ggml-backend.h"
    "${_hmi_ggml_root}/include/ggml-cpp.h"
    "${_hmi_ggml_root}/include/gguf.h")
  if(NOT EXISTS "${_hmi_ggml_file}")
    message(FATAL_ERROR "ggml diagnostic source dependency is missing: ${_hmi_ggml_file}")
  endif()
endforeach()

find_package(Threads REQUIRED)
if(HMI_BUILD_AUDIOCPP_FROM_SOURCE)
  add_library(hmi_ggml_kernel_base ALIAS ggml-base)
  add_library(hmi_ggml_kernel_core ALIAS ggml)
  set(_hmi_probe_runtime "${HMI_AUDIOCPP_RUNTIME_SOURCE}")
else()
  foreach(_hmi_ggml_file IN ITEMS
    "${HMI_GGML_SDK_ROOT}/bin/ggml.dll"
    "${HMI_GGML_SDK_ROOT}/bin/ggml-base.dll"
    "${HMI_GGML_SDK_ROOT}/lib/ggml.lib"
    "${HMI_GGML_SDK_ROOT}/lib/ggml-base.lib")
  if(NOT EXISTS "${_hmi_ggml_file}")
    message(FATAL_ERROR "ggml diagnostic dependency is missing: ${_hmi_ggml_file}. Use the matching SDK source, DLLs and import libraries.")
  endif()
endforeach()
add_library(hmi_ggml_kernel_base SHARED IMPORTED)
set_target_properties(hmi_ggml_kernel_base PROPERTIES
  IMPORTED_LOCATION "${HMI_GGML_SDK_ROOT}/bin/ggml-base.dll"
  IMPORTED_IMPLIB "${HMI_GGML_SDK_ROOT}/lib/ggml-base.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${_hmi_ggml_root}/include"
  INTERFACE_COMPILE_DEFINITIONS "GGML_SHARED;GGML_BACKEND_SHARED")
add_library(hmi_ggml_kernel_core SHARED IMPORTED)
set_target_properties(hmi_ggml_kernel_core PROPERTIES
  IMPORTED_LOCATION "${HMI_GGML_SDK_ROOT}/bin/ggml.dll"
  IMPORTED_IMPLIB "${HMI_GGML_SDK_ROOT}/lib/ggml.lib"
  INTERFACE_LINK_LIBRARIES hmi_ggml_kernel_base)
  set(_hmi_probe_runtime "${HMI_GGML_SDK_ROOT}/bin")
endif()

# This is the upstream single-file diagnostic, never an HMI/model dependency.
add_executable(hmi_ggml_backend_ops EXCLUDE_FROM_ALL "${_hmi_ggml_ops_source}")
target_link_libraries(hmi_ggml_backend_ops PRIVATE hmi_ggml_kernel_core Threads::Threads)
target_compile_definitions(hmi_ggml_backend_ops PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
set_target_properties(hmi_ggml_backend_ops PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/diagnostics/ggml-kernels/bin/$<CONFIG>/runtime")
if(MSVC)
  # Force-include before upstream headers so Release assertions stay active
  # without cl's /D versus /U override warning or changing global flags.
  set(_hmi_ggml_assertions "${CMAKE_CURRENT_BINARY_DIR}/diagnostics/ggml-kernels/enable-assertions.h")
  file(GENERATE OUTPUT "${_hmi_ggml_assertions}" CONTENT "#ifdef NDEBUG\n#undef NDEBUG\n#endif\n")
  target_compile_options(hmi_ggml_backend_ops PRIVATE "/FI${_hmi_ggml_assertions}" /utf-8 /permissive-)
else()
  target_compile_options(hmi_ggml_backend_ops PRIVATE -UNDEBUG)
endif()

# Synchronize the exact plugin set even when no executable needs relinking.
add_custom_target(hmi_ggml_runtime
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${_hmi_probe_runtime}"
    "-DDESTINATION_DIR=$<TARGET_FILE_DIR:hmi_ggml_backend_ops>"
    "-DREQUIRE_MANIFEST=ON"
    -P "${CMAKE_CURRENT_LIST_DIR}/DeployRuntime.cmake"
  VERBATIM)
if(HMI_BUILD_AUDIOCPP_FROM_SOURCE)
  add_dependencies(hmi_ggml_runtime hmi_audio_sdk)
endif()
add_dependencies(hmi_ggml_backend_ops hmi_ggml_runtime)

foreach(_hmi_ggml_mode IN ITEMS support perf)
  if(_hmi_ggml_mode STREQUAL "support")
    set(_hmi_ggml_params "${HMI_GGML_SUPPORT_PARAMS}")
  else()
    set(_hmi_ggml_params "${HMI_GGML_PERF_PARAMS}")
  endif()
  add_custom_target(hmi_ggml_kernel_${_hmi_ggml_mode}
    COMMAND ${CMAKE_COMMAND}
      "-DPROBE_EXECUTABLE=$<TARGET_FILE:hmi_ggml_backend_ops>"
      "-DPROBE_MODE=${_hmi_ggml_mode}"
      "-DPROBE_BACKEND=${HMI_GGML_KERNEL_BACKEND}"
      "-DPROBE_OPS=${HMI_GGML_KERNEL_OPS}"
      "-DPROBE_PARAMS=${_hmi_ggml_params}"
      "-DPROBE_TIMEOUT=${HMI_GGML_KERNEL_TIMEOUT}"
      "-DPROBE_OUTPUT_DIR=${CMAKE_BINARY_DIR}/diagnostics/ggml-kernels/$<CONFIG>"
      -P "${CMAKE_CURRENT_LIST_DIR}/RunGgmlKernelProbe.cmake"
    DEPENDS hmi_ggml_backend_ops
    COMMENT "Running official ggml ${_hmi_ggml_mode} on ${HMI_GGML_KERNEL_BACKEND}"
    USES_TERMINAL VERBATIM)
endforeach()

# A bounded graph probe complements the upstream exhaustive operator cases.
# It exposes dynamic CPU thread control and independent numeric verification.
add_executable(hmi_ggml_probe EXCLUDE_FROM_ALL "${PROJECT_SOURCE_DIR}/tools/ggml_probe.cpp")
target_compile_features(hmi_ggml_probe PRIVATE cxx_std_23)
target_link_libraries(hmi_ggml_probe PRIVATE hmi_ggml_kernel_core Threads::Threads)
target_compile_definitions(hmi_ggml_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(hmi_ggml_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:hmi_ggml_backend_ops>")
if(MSVC)
  target_compile_options(hmi_ggml_probe PRIVATE /W4 /permissive- /utf-8)
endif()
add_dependencies(hmi_ggml_probe hmi_ggml_runtime)
set(HMI_GGML_MATRIX_BACKENDS "auto" CACHE STRING "auto discovers devices; otherwise comma-separated exact device names")
set(HMI_GGML_MATRIX_THREADS "1,2,4" CACHE STRING "CPU threads to compare, 1..256")
set(HMI_GGML_MATRIX_VARIANTS "auto" CACHE STRING "auto, all compatible CPU variants, or comma-separated CPU DLL filenames")
set(HMI_GGML_MATRIX_OPS "matmul,softmax" CACHE STRING "matmul,softmax or either bounded graph")
set(HMI_GGML_MATRIX_SIZE "256" CACHE STRING "Square graph side, 32..1024")
set(HMI_GGML_MATRIX_RUNS "5" CACHE STRING "Timed runs after one warmup, 1..100")
set(HMI_GGML_MATRIX_OMP_WAIT "PASSIVE" CACHE STRING "Comma-separated OpenMP wait policies ACTIVE,PASSIVE")
foreach(_hmi_mode inventory perf_matrix)
  add_custom_target(hmi_ggml_${_hmi_mode}
    COMMAND "${CMAKE_COMMAND}"
      "-DPROBE_EXECUTABLE=$<TARGET_FILE:hmi_ggml_probe>"
      "-DPROBE_MODE=${_hmi_mode}"
      "-DPROBE_BACKENDS=${HMI_GGML_MATRIX_BACKENDS}"
      "-DPROBE_THREADS=${HMI_GGML_MATRIX_THREADS}"
      "-DPROBE_VARIANTS=${HMI_GGML_MATRIX_VARIANTS}"
      "-DPROBE_OPS=${HMI_GGML_MATRIX_OPS}"
      "-DPROBE_SIZE=${HMI_GGML_MATRIX_SIZE}"
      "-DPROBE_RUNS=${HMI_GGML_MATRIX_RUNS}"
      "-DPROBE_OMP_WAIT=${HMI_GGML_MATRIX_OMP_WAIT}"
      "-DPROBE_TIMEOUT=${HMI_GGML_KERNEL_TIMEOUT}"
      "-DPROBE_OUTPUT_DIR=${CMAKE_BINARY_DIR}/diagnostics/ggml-kernels/$<CONFIG>"
      -P "${CMAKE_CURRENT_LIST_DIR}/RunGgmlPerformance.cmake"
    DEPENDS hmi_ggml_probe
    USES_TERMINAL VERBATIM)
endforeach()
add_executable(hmi_ggml_isa_policy_tests EXCLUDE_FROM_ALL "${PROJECT_SOURCE_DIR}/tests/ggml_isa_policy.cpp")
target_include_directories(hmi_ggml_isa_policy_tests PRIVATE "${PROJECT_SOURCE_DIR}/tools")
if(MSVC)
  target_compile_options(hmi_ggml_isa_policy_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_custom_target(hmi_ggml_probe_check
  COMMAND "$<TARGET_FILE:hmi_ggml_isa_policy_tests>"
  COMMAND "${CMAKE_COMMAND}" "-DPROBE_EXECUTABLE=$<TARGET_FILE:hmi_ggml_probe>"
    -P "${PROJECT_SOURCE_DIR}/tests/ggml_probe_validation.cmake"
  DEPENDS hmi_ggml_probe hmi_ggml_isa_policy_tests
  USES_TERMINAL VERBATIM)

message(STATUS "ggml kernel diagnostics enabled for ${HMI_GGML_KERNEL_BACKEND}; run hmi_ggml_kernel_support or hmi_ggml_kernel_perf explicitly")
