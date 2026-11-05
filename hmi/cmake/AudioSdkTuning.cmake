include_guard(GLOBAL)

# These targets build a separate SDK. No model implementation enters HMI.
option(HMI_ENABLE_AUDIOCPP_SDK_BUILD "Expose explicit native SDK tuning/configure/build targets" OFF)
if(NOT HMI_ENABLE_AUDIOCPP_SDK_BUILD)
  return()
endif()
if(HMI_BUILD_AUDIOCPP_FROM_SOURCE AND TARGET hmi_audio_sdk)
  add_custom_target(hmi_native_sdk_configure
    COMMAND "${CMAKE_COMMAND}" -E echo "The native SDK is configured from bundled project sources. Change HMI_AUDIOCPP_SDK_* and reconfigure this build to tune it."
    VERBATIM)
  add_custom_target(hmi_native_sdk_build DEPENDS hmi_audio_sdk)
  add_custom_target(hmi_audiocpp_sdk_configure DEPENDS hmi_native_sdk_configure)
  add_custom_target(hmi_audiocpp_sdk_build DEPENDS hmi_native_sdk_build)
  return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/CompilerCache.cmake" NO_POLICY_SCOPE)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "SDK CPU tuning currently supports Windows x64.")
endif()
set(HMI_AUDIOCPP_SOURCE_ROOT "${PROJECT_SOURCE_DIR}/3rd_party/native/source" CACHE PATH "Bundled standalone SDK source")
set(HMI_AUDIOCPP_CPU_PROFILE "portable" CACHE STRING "SDK CPU ISA profile; portable builds runtime-dispatched variants")
set_property(CACHE HMI_AUDIOCPP_CPU_PROFILE PROPERTY STRINGS portable x64 sse42 avx avx2 avx-vnni avx512 avx512-vnni)
option(HMI_AUDIOCPP_SDK_OPENMP "Enable both SDK host and ggml OpenMP" ON)
option(HMI_AUDIOCPP_SDK_CUDA "Enable CUDA in the separately built SDK" OFF)
option(HMI_AUDIOCPP_SDK_CUDNN "Enable cuDNN convolution in the separate CUDA SDK" OFF)
option(HMI_AUDIOCPP_SDK_VULKAN "Enable Vulkan in the separately built SDK" ON)
set(HMI_CUDNN_ROOT "" CACHE PATH "Optional cuDNN installation or flat archive root")
set(HMI_CUDNN_CUDA_VERSION "" CACHE STRING "Optional cuDNN CUDA major.minor; must match the selected toolkit")
set(HMI_CUDNN_ARCHIVE_CUDA_VERSION "" CACHE STRING "CUDA family/version declaration for a flat archive without a cuda13 name")
set(HMI_AUDIOCPP_SDK_CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES_NATIVE}" CACHE STRING "CUDA architectures for the independent SDK; default uses the detected GPU")
set(HMI_AUDIOCPP_SDK_JOBS "4" CACHE STRING "Maximum parallel SDK build jobs")
set(_hmi_sdk_profiles portable x64 sse42 avx avx2 avx-vnni avx512 avx512-vnni)
if(NOT HMI_AUDIOCPP_CPU_PROFILE IN_LIST _hmi_sdk_profiles)
  message(FATAL_ERROR "Unknown HMI_AUDIOCPP_CPU_PROFILE=${HMI_AUDIOCPP_CPU_PROFILE}")
endif()
if(NOT HMI_AUDIOCPP_SDK_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "HMI_AUDIOCPP_SDK_JOBS must be positive.")
endif()
set(_hmi_sdk_cudnn_flags "-DGGML_CUDA_CUDNN_CONV2D=OFF")
set(_hmi_cudnn_identity "OFF")
set(_hmi_cudnn_metadata "{\"enabled\":false}")
set(HMI_CUDNN_RUNTIME_BIN "")
if(HMI_AUDIOCPP_SDK_CUDNN)
  if(NOT HMI_AUDIOCPP_SDK_CUDA)
    message(FATAL_ERROR "HMI_AUDIOCPP_SDK_CUDNN requires HMI_AUDIOCPP_SDK_CUDA=ON")
  endif()
  find_package(CUDAToolkit REQUIRED)
  string(REGEX MATCH "^[0-9]+\\.[0-9]+" _hmi_selected_cuda "${CUDAToolkit_VERSION}")
  if(NOT _hmi_selected_cuda)
    message(FATAL_ERROR "Cannot identify the selected CUDA toolkit version for cuDNN")
  endif()
  if(HMI_CUDNN_CUDA_VERSION AND NOT HMI_CUDNN_CUDA_VERSION STREQUAL _hmi_selected_cuda)
    message(FATAL_ERROR "cuDNN CUDA version ${HMI_CUDNN_CUDA_VERSION} does not match selected toolkit ${_hmi_selected_cuda}")
  endif()
  include("${CMAKE_CURRENT_LIST_DIR}/FindHmiCudnn.cmake")
  hmi_find_cudnn(_hmi_cudnn ROOT "${HMI_CUDNN_ROOT}" CUDA_VERSION "${_hmi_selected_cuda}"
    ARCHIVE_CUDA_VERSION "${HMI_CUDNN_ARCHIVE_CUDA_VERSION}")
  if(NOT _hmi_cudnn_FOUND)
    message(FATAL_ERROR "${_hmi_cudnn_ERROR}")
  endif()
  set(HMI_CUDNN_RUNTIME_BIN "${_hmi_cudnn_RUNTIME_BIN}")
  set(HMI_CUDNN_INCLUDE_DIR "${_hmi_cudnn_INCLUDE_DIR}")
  set(HMI_CUDNN_LIBRARY "${_hmi_cudnn_LIBRARY}")
  set(_hmi_sdk_cudnn_flags "-DGGML_CUDA_CUDNN_CONV2D=ON"
    "-DCUDNN_INCLUDE_DIR=${_hmi_cudnn_INCLUDE_DIR}" "-DCUDNN_LIBRARY=${_hmi_cudnn_LIBRARY}")
  if(CUDAToolkit_NVCC_EXECUTABLE)
    list(APPEND _hmi_sdk_cudnn_flags "-DCMAKE_CUDA_COMPILER=${CUDAToolkit_NVCC_EXECUTABLE}")
  endif()
  file(SHA256 "${_hmi_cudnn_LIBRARY}" _hmi_cudnn_library_hash)
  set(_hmi_cudnn_identity "ON|${_hmi_cudnn_VERSION}|${_hmi_selected_cuda}|${_hmi_cudnn_ROOT}|${_hmi_cudnn_INCLUDE_DIR}|${_hmi_cudnn_LIBRARY}|${_hmi_cudnn_RUNTIME_BIN}|${_hmi_cudnn_library_hash}")
  set(_hmi_cudnn_metadata "{\"enabled\":true}")
  foreach(_entry ROOT INCLUDE_DIR LIBRARY RUNTIME_BIN VERSION CUDA_VERSION)
    string(TOLOWER "${_entry}" _key)
    string(REPLACE "\\" "\\\\" _value "${_hmi_cudnn_${_entry}}")
    string(REPLACE "\"" "\\\"" _value "${_value}")
    string(JSON _hmi_cudnn_metadata SET "${_hmi_cudnn_metadata}" "${_key}" "\"${_value}\"")
  endforeach()
  set(_hmi_cudnn_runtime_json "[]")
  set(_hmi_index 0)
  foreach(_file IN LISTS _hmi_cudnn_RUNTIME_FILES)
    string(REPLACE "\\" "\\\\" _value "${_file}")
    string(REPLACE "\"" "\\\"" _value "${_value}")
    string(JSON _hmi_cudnn_runtime_json SET "${_hmi_cudnn_runtime_json}" ${_hmi_index} "\"${_value}\"")
    math(EXPR _hmi_index "${_hmi_index} + 1")
  endforeach()
  string(JSON _hmi_cudnn_metadata SET "${_hmi_cudnn_metadata}" runtime_files "${_hmi_cudnn_runtime_json}")
  message(STATUS "SDK cuDNN ${_hmi_cudnn_VERSION} for CUDA ${_hmi_selected_cuda}: ${_hmi_cudnn_LIBRARY}; runtime ${HMI_CUDNN_RUNTIME_BIN}")
endif()
set(HMI_AUDIOCPP_SDK_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/sdk" CACHE PATH "Root of isolated SDK profile builds")
include("${CMAKE_CURRENT_LIST_DIR}/ProjectPaths.cmake")
hmi_require_project_path("${HMI_AUDIOCPP_SOURCE_ROOT}" "native source")
hmi_require_build_path("${HMI_AUDIOCPP_SDK_OUTPUT_ROOT}" "SDK output")
file(REAL_PATH "${HMI_AUDIOCPP_SOURCE_ROOT}" _hmi_sdk_source_real)
string(SHA256 _hmi_sdk_identity "${_hmi_sdk_source_real}|${CMAKE_GENERATOR}|${CMAKE_GENERATOR_PLATFORM}|${CMAKE_GENERATOR_TOOLSET}|${CMAKE_C_COMPILER}|${CMAKE_CXX_COMPILER}|${CMAKE_CXX_COMPILER_VERSION}|${HMI_AUDIOCPP_CPU_PROFILE}|${HMI_AUDIOCPP_SDK_OPENMP}|${HMI_AUDIOCPP_SDK_CUDA}|${HMI_AUDIOCPP_SDK_VULKAN}|${HMI_AUDIOCPP_SDK_CUDA_ARCHITECTURES}|${_hmi_cudnn_identity}|${HMI_USE_CCACHE}|${HMI_CCACHE_EXECUTABLE}|${HMI_CCACHE_DIR}")
string(SUBSTRING "${_hmi_sdk_identity}" 0 12 _hmi_sdk_identity)
set(HMI_AUDIOCPP_SDK_BUILD_DIR "${HMI_AUDIOCPP_SDK_OUTPUT_ROOT}/${HMI_AUDIOCPP_CPU_PROFILE}-${_hmi_sdk_identity}")
set(_hmi_sdk_initial_cache "${CMAKE_BINARY_DIR}/sdk-compiler-cache.cmake")
hmi_write_ccache_initial_cache("${_hmi_sdk_initial_cache}")
set(HMI_CUDNN_METADATA_FILE "${CMAKE_BINARY_DIR}/sdk-cudnn.json")
string(REPLACE "\\" "\\\\" _hmi_sdk_json_directory "${HMI_AUDIOCPP_SDK_BUILD_DIR}")
string(REPLACE "\"" "\\\"" _hmi_sdk_json_directory "${_hmi_sdk_json_directory}")
string(JSON _hmi_cudnn_metadata SET "${_hmi_cudnn_metadata}" sdk_build_directory "\"${_hmi_sdk_json_directory}\"")
file(GENERATE OUTPUT "${HMI_CUDNN_METADATA_FILE}" CONTENT "${_hmi_cudnn_metadata}\n")
set(_hmi_sdk_generator -G "${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
  list(APPEND _hmi_sdk_generator -A "${CMAKE_GENERATOR_PLATFORM}")
endif()
if(CMAKE_GENERATOR_TOOLSET)
  list(APPEND _hmi_sdk_generator -T "${CMAKE_GENERATOR_TOOLSET}")
endif()
if(CMAKE_MAKE_PROGRAM)
  list(APPEND _hmi_sdk_generator "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
endif()
set(_hmi_sdk_cuda_flags "")
set(_hmi_sdk_cuda_arch "")
if(NOT HMI_AUDIOCPP_SDK_CUDA_ARCHITECTURES STREQUAL "")
  string(REPLACE ";" "\\;" _hmi_sdk_cuda_arch "${HMI_AUDIOCPP_SDK_CUDA_ARCHITECTURES}")
  list(APPEND _hmi_sdk_cuda_flags "-DCMAKE_CUDA_ARCHITECTURES=${_hmi_sdk_cuda_arch}")
endif()
# Configure the project wrapper so native naming and matching staging also apply
# to explicitly tuned SDKs while the parent application uses a prebuilt set.
add_custom_target(hmi_native_sdk_configure
  COMMAND "${CMAKE_COMMAND}" -S "${PROJECT_SOURCE_DIR}" -B "${HMI_AUDIOCPP_SDK_BUILD_DIR}"
    ${_hmi_sdk_generator}
    -C "${_hmi_sdk_initial_cache}"
    "-DCMAKE_BUILD_TYPE=Release"
    "-DHMI_BUILD_AUDIOCPP_FROM_SOURCE=ON" "-DHMI_ENABLE_AUDIOCPP_SDK_BUILD=OFF"
    "-DHMI_BUILD_GGML_KERNEL_PROBE=OFF"
    "-DHMI_AUDIOCPP_SOURCE_ROOT=${HMI_AUDIOCPP_SOURCE_ROOT}"
    "-DHMI_AUDIOCPP_CPU_PROFILE=${HMI_AUDIOCPP_CPU_PROFILE}"
    "-DHMI_ENABLE_CUDA=${HMI_AUDIOCPP_SDK_CUDA}"
    "-DHMI_AUDIOCPP_SDK_OPENMP=${HMI_AUDIOCPP_SDK_OPENMP}"
    "-DHMI_AUDIOCPP_SDK_CUDA=${HMI_AUDIOCPP_SDK_CUDA}"
    "-DHMI_AUDIOCPP_SDK_VULKAN=${HMI_AUDIOCPP_SDK_VULKAN}"
    "-DHMI_AUDIOCPP_SDK_CUDNN=${HMI_AUDIOCPP_SDK_CUDNN}"
    "-DHMI_AUDIOCPP_SDK_CUDA_ARCHITECTURES=${_hmi_sdk_cuda_arch}"
    "-DHMI_CUDNN_ROOT=${HMI_CUDNN_ROOT}"
    "-DHMI_CUDNN_CUDA_VERSION=${HMI_CUDNN_CUDA_VERSION}"
    "-DHMI_CUDNN_ARCHIVE_CUDA_VERSION=${HMI_CUDNN_ARCHIVE_CUDA_VERSION}"
    ${_hmi_sdk_cuda_flags}
    ${_hmi_sdk_cudnn_flags}
  COMMENT "Configuring independent native SDK (${HMI_AUDIOCPP_CPU_PROFILE}, OpenMP=${HMI_AUDIOCPP_SDK_OPENMP})"
  USES_TERMINAL VERBATIM)
add_custom_target(hmi_native_sdk_build
  COMMAND "${CMAKE_COMMAND}" --build "${HMI_AUDIOCPP_SDK_BUILD_DIR}" --config Release --target hmi_audio_sdk --parallel "${HMI_AUDIOCPP_SDK_JOBS}"
  DEPENDS hmi_native_sdk_configure
  COMMENT "Building independent tuned native SDK; deployment is explicit"
  USES_TERMINAL VERBATIM)
add_custom_target(hmi_audiocpp_sdk_configure DEPENDS hmi_native_sdk_configure)
add_custom_target(hmi_audiocpp_sdk_build DEPENDS hmi_native_sdk_build)
message(STATUS "SDK tuning enabled: ${HMI_AUDIOCPP_CPU_PROFILE}; OpenMP=${HMI_AUDIOCPP_SDK_OPENMP}; output ${HMI_AUDIOCPP_SDK_BUILD_DIR}")
