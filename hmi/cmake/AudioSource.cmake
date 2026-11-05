include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/ProjectPaths.cmake")

# The application consumes the public C ABI; model code belongs to the separate
# native DLL target, built from the sources copied into this project.
option(HMI_BUILD_AUDIOCPP_FROM_SOURCE "Build the bundled native SDK with the application" ON)
if(DEFINED HMI_AUDIOCPP_SOURCE_ROOT)
  file(TO_CMAKE_PATH "${HMI_AUDIOCPP_SOURCE_ROOT}" _hmi_previous_source)
  cmake_path(ABSOLUTE_PATH _hmi_previous_source BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE _hmi_previous_source)
  string(TOLOWER "${_hmi_previous_source}" _hmi_previous_source)
  string(TOLOWER "${PROJECT_SOURCE_DIR}/3rd_party/audiocpp/source" _hmi_legacy_source)
  if(_hmi_previous_source STREQUAL _hmi_legacy_source)
    set(HMI_AUDIOCPP_SOURCE_ROOT "${PROJECT_SOURCE_DIR}/3rd_party/native/source" CACHE PATH "Bundled native dependency source" FORCE)
    set(HMI_AUDIOCPP_SOURCE_ROOT "${PROJECT_SOURCE_DIR}/3rd_party/native/source")
  endif()
endif()
set(HMI_AUDIOCPP_SOURCE_ROOT "${PROJECT_SOURCE_DIR}/3rd_party/native/source" CACHE PATH "Bundled native dependency source")

function(_hmi_audio_collect_targets directory result)
  get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(_child IN LISTS _children)
    _hmi_audio_collect_targets("${_child}" _nested)
    list(APPEND _targets ${_nested})
  endforeach()
  set(${result} "${_targets}" PARENT_SCOPE)
endfunction()

function(hmi_track_audio_model_specs source_directory)
  # Upstream embeds JSON read at configure time. GLOB CONFIGURE_DEPENDS alone
  # notices added/removed filenames, but not edits to an existing specification.
  file(GLOB _specifications CONFIGURE_DEPENDS "${source_directory}/model_specs/*.json")
  # Register on the still-active caller directory. CMake has already collected
  # the child directory's dependencies when add_subdirectory() returns.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_specifications})
endfunction()

function(hmi_migrate_native_shader_cache binary_directory)
  set(_shader_cache "${binary_directory}/ggml/upstream/src/ggml-vulkan/vulkan-shaders-gen-prefix/src/vulkan-shaders-gen-build/CMakeCache.txt")
  if(NOT EXISTS "${_shader_cache}")
    return()
  endif()
  file(STRINGS "${_shader_cache}" _shader_home REGEX "^CMAKE_HOME_DIRECTORY:INTERNAL=")
  string(REGEX REPLACE "^CMAKE_HOME_DIRECTORY:INTERNAL=" "" _shader_home "${_shader_home}")
  file(TO_CMAKE_PATH "${_shader_home}" _shader_home)
  string(TOLOWER "${_shader_home}" _shader_home)
  file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/3rd_party/audiocpp/source/external/ggml/src/ggml-vulkan/vulkan-shaders" _legacy_home)
  string(TOLOWER "${_legacy_home}" _legacy_home)
  if(_shader_home STREQUAL _legacy_home)
    hmi_require_build_path("${_shader_cache}" "Native shader cache")
    file(REMOVE "${_shader_cache}")
    message(STATUS "Reset legacy native shader source-directory cache")
  endif()
endfunction()

function(hmi_add_audio_source_sdk)
  if(NOT HMI_BUILD_AUDIOCPP_FROM_SOURCE)
    # Reuse the complete Release SDK; source directories never store binaries.
    set(_prebuilt "${PROJECT_SOURCE_DIR}/build/audio-sdk/package/Release")
    hmi_require_build_path("${_prebuilt}" "Prebuilt native SDK")
    foreach(_required include/audiocpp.h sdk-manifest.json)
      if(NOT EXISTS "${_prebuilt}/${_required}")
        message(FATAL_ERROR "Prebuilt native SDK is missing: ${_prebuilt}/${_required}. Build hmi_audio_sdk with HMI_BUILD_AUDIOCPP_FROM_SOURCE=ON first.")
      endif()
    endforeach()
    set(HMI_AUDIOCPP_RUNTIME_SOURCE "${_prebuilt}/bin" PARENT_SCOPE)
    set(HMI_AUDIOCPP_HEADER_DIR "${_prebuilt}/include" PARENT_SCOPE)
    return()
  endif()
  if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The bundled application SDK currently requires Windows x64.")
  endif()
  hmi_require_project_path("${HMI_AUDIOCPP_SOURCE_ROOT}" "native source")
  if(NOT EXISTS "${HMI_AUDIOCPP_SOURCE_ROOT}/include/audiocpp.h")
    message(FATAL_ERROR "Bundled native sources are missing: ${HMI_AUDIOCPP_SOURCE_ROOT}")
  endif()
  set(HMI_AUDIOCPP_CPU_PROFILE "portable" CACHE STRING "CPU implementation to build in the bundled SDK")
  set_property(CACHE HMI_AUDIOCPP_CPU_PROFILE PROPERTY STRINGS portable x64 sse42 avx avx2 avx-vnni avx512 avx512-vnni)
  option(HMI_AUDIOCPP_SDK_CUDA "Build CUDA in the bundled SDK" ${HAS_CUDA})
  option(HMI_AUDIOCPP_SDK_VULKAN "Build Vulkan in the bundled SDK" ON)
  option(HMI_AUDIOCPP_SDK_OPENMP "Build OpenMP in the bundled SDK" ${HMI_ENABLE_OPENMP})
  option(HMI_AUDIOCPP_SDK_CUDNN "Use cuDNN convolution in the bundled CUDA SDK" OFF)
  # A fresh local build should target its detected GPU instead of silently
  # inheriting NVCC's baseline (sm75). Explicit/cached SDK choices still win.
  set(_hmi_default_cuda_architectures "${CMAKE_CUDA_ARCHITECTURES_NATIVE}")
  set(HMI_AUDIOCPP_SDK_CUDA_ARCHITECTURES "${_hmi_default_cuda_architectures}" CACHE STRING "Bundled SDK CUDA architectures; default uses the detected GPU")
  set(_cudnn_bin "")
  set(_cudnn_license "")
  set(_cudnn_version "")
  if(HMI_AUDIOCPP_SDK_CUDNN)
    if(NOT HMI_AUDIOCPP_SDK_CUDA)
      message(FATAL_ERROR "HMI_AUDIOCPP_SDK_CUDNN requires HMI_AUDIOCPP_SDK_CUDA=ON")
    endif()
    find_package(CUDAToolkit REQUIRED)
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" _cuda_version "${CUDAToolkit_VERSION}")
    set(HMI_CUDNN_ROOT "" CACHE PATH "Optional cuDNN installation/archive root")
    set(HMI_CUDNN_CUDA_VERSION "" CACHE STRING "Optional cuDNN CUDA version; must match the selected toolkit")
    set(HMI_CUDNN_ARCHIVE_CUDA_VERSION "" CACHE STRING "CUDA version declaration for a flat cuDNN archive")
    if(HMI_CUDNN_CUDA_VERSION AND NOT HMI_CUDNN_CUDA_VERSION STREQUAL _cuda_version)
      message(FATAL_ERROR "cuDNN CUDA version does not match the selected toolkit ${_cuda_version}")
    endif()
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/FindHmiCudnn.cmake")
    hmi_find_cudnn(_cudnn ROOT "${HMI_CUDNN_ROOT}" CUDA_VERSION "${_cuda_version}"
      ARCHIVE_CUDA_VERSION "${HMI_CUDNN_ARCHIVE_CUDA_VERSION}")
    if(NOT _cudnn_FOUND)
      message(FATAL_ERROR "${_cudnn_ERROR}")
    endif()
    set(CUDNN_INCLUDE_DIR "${_cudnn_INCLUDE_DIR}" CACHE PATH "Matching cuDNN headers" FORCE)
    set(CUDNN_LIBRARY "${_cudnn_LIBRARY}" CACHE FILEPATH "Matching cuDNN import library" FORCE)
    set(_cudnn_bin "${_cudnn_RUNTIME_BIN}")
    set(_cudnn_license "${_cudnn_ROOT}/LICENSE")
    set(_cudnn_version "${_cudnn_VERSION}")
  endif()
  # Normal variables keep this source subtree's configuration private, including
  # when upstream deliberately forces the corresponding cache entries.
  set(BUILD_SHARED_LIBS ON)
  set(ENGINE_ENABLE_CPU_ALL_VARIANTS ON CACHE BOOL "Build guarded CPU implementations" FORCE)
  set(ENGINE_ENABLE_NATIVE_CPU OFF CACHE BOOL "Keep host code portable" FORCE)
  foreach(_feature CUDA VULKAN OPENMP)
    set(ENGINE_ENABLE_${_feature} "${HMI_AUDIOCPP_SDK_${_feature}}" CACHE BOOL "Bundled SDK backend" FORCE)
  endforeach()
  set(GGML_OPENMP "${HMI_AUDIOCPP_SDK_OPENMP}" CACHE BOOL "SDK OpenMP" FORCE)
  set(GGML_CUDA_CUDNN_CONV2D "${HMI_AUDIOCPP_SDK_CUDNN}" CACHE BOOL "SDK cuDNN convolution" FORCE)
  foreach(_feature HIP METAL NEON RKNN)
    set(ENGINE_ENABLE_${_feature} OFF CACHE BOOL "Unsupported SDK backend" FORCE)
  endforeach()
  foreach(_feature EXAMPLES TESTS EXTENDED_TESTS MODEL_TESTS WARMBENCH)
    set(ENGINE_BUILD_${_feature} OFF CACHE BOOL "Application SDK excludes upstream tools/tests" FORCE)
  endforeach()
  set(AUDIOCPP_MODEL_SET custom CACHE STRING "Application model dependencies" FORCE)
  set(AUDIOCPP_MODELS "qwen3_asr;qwen3_tts" CACHE STRING "Application model dependencies" FORCE)
  set(SPM_BUILD_TEST OFF CACHE BOOL "Native subset excludes vendor tests" FORCE)
  set(SPM_ENABLE_NFKC_COMPILE OFF CACHE BOOL "Native subset uses bundled normalization data" FORCE)
  set(AUDIOCPP_BUILD_C_API ON CACHE BOOL "Build public SDK facade" FORCE)
  set(AUDIOCPP_DEPLOYMENT_BUILD ON CACHE BOOL "Embed matching model specifications" FORCE)
  set(AUDIOCPP_STATIC_ESPEAK OFF CACHE BOOL "No optional speech dependency" FORCE)
  set(AUDIOCPP_BUILD_NATIVE_MODEL_MANAGER OFF CACHE BOOL "No model manager" FORCE)
  set(AUDIOCPP_VERSION "dev" CACHE STRING "Local native build label" FORCE)
  set(HMI_SDK_CPU_PROFILE "${HMI_AUDIOCPP_CPU_PROFILE}")
  set(HMI_GGML_UPSTREAM_DIR "${HMI_AUDIOCPP_SOURCE_ROOT}/external/ggml")
  set(AUDIOCPP_GGML_SOURCE_DIR "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sdk-ggml" CACHE PATH "Project ggml configuration adapter" FORCE)
  if(HMI_AUDIOCPP_SDK_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "${HMI_AUDIOCPP_SDK_CUDA_ARCHITECTURES}")
  endif()
  set(_source_binary "${CMAKE_BINARY_DIR}/audio-sdk/build")
  hmi_migrate_native_shader_cache("${_source_binary}")
  if(MSVC AND HMI_AUDIOCPP_SDK_OPENMP)
    # Match upstream's SIMD-capable mode in the imported OpenMP target so MSVC
    # receives one consistent option instead of an /openmp override warning.
    set(OpenMP_CXX_FLAGS "/openmp:experimental")
  endif()
  add_subdirectory("${HMI_AUDIOCPP_SOURCE_ROOT}" "${_source_binary}" EXCLUDE_FROM_ALL)
  # Keep the upstream C ABI and target while branding the delivered facade.
  set_target_properties(audiocpp PROPERTIES OUTPUT_NAME native)
  if(MSVC AND HMI_AUDIOCPP_SDK_OPENMP AND TARGET engine_core)
    # MSVC supports the outer parallel loop and SIMD but ignores collapse.
    # Limit this known compatibility diagnostic to the vendored core target;
    # application warnings and all other SDK diagnostics remain enabled.
    target_compile_options(engine_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/wd4849>)
  endif()
  hmi_track_audio_model_specs("${HMI_AUDIOCPP_SOURCE_ROOT}")
  _hmi_audio_collect_targets("${HMI_AUDIOCPP_SOURCE_ROOT}" _sdk_targets)
  set(_runtime_targets)
  set(_runtime_files)
  foreach(_target IN LISTS _sdk_targets)
    get_target_property(_type "${_target}" TYPE)
    if(_type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "MODULE_LIBRARY")
      set_target_properties("${_target}" PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/audio-sdk/bin/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/audio-sdk/bin/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/audio-sdk/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/audio-sdk/pdb/$<CONFIG>")
      list(APPEND _runtime_targets "${_target}")
      list(APPEND _runtime_files "$<TARGET_FILE:${_target}>")
    endif()
  endforeach()
  set(_package "${CMAKE_BINARY_DIR}/audio-sdk/package/$<CONFIG>")
  set(_inputs "${CMAKE_BINARY_DIR}/audio-sdk/stage-inputs-$<CONFIG>.cmake")
  file(GENERATE OUTPUT "${_inputs}" CONTENT "set(SDK_RUNTIME_FILES [==[${_runtime_files}]==])\n")
  add_custom_target(hmi_audio_sdk
    COMMAND "${CMAKE_COMMAND}"
      "-DSDK_RAW_BIN=$<TARGET_FILE_DIR:audiocpp>"
      "-DSDK_PACKAGE_ROOT=${_package}"
      "-DSDK_SOURCE=${HMI_AUDIOCPP_SOURCE_ROOT}"
      "-DSDK_INPUTS_FILE=${_inputs}"
      "-DAUDIOCPP_IMPORT=$<TARGET_LINKER_FILE:audiocpp>"
      "-DGGML_IMPORT=$<TARGET_LINKER_FILE:ggml>"
      "-DGGML_BASE_IMPORT=$<TARGET_LINKER_FILE:ggml-base>"
      "-DCUDNN_BIN=${_cudnn_bin}" "-DCUDNN_LICENSE=${_cudnn_license}" "-DCUDNN_VERSION=${_cudnn_version}"
      -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageAudioSdk.cmake"
    DEPENDS ${_runtime_targets}
    COMMENT "Staging the native SDK built from project sources"
    VERBATIM)
  set(HMI_AUDIOCPP_RUNTIME_SOURCE "${_package}/bin" PARENT_SCOPE)
  set(HMI_AUDIOCPP_HEADER_DIR "${HMI_AUDIOCPP_SOURCE_ROOT}/include" PARENT_SCOPE)
  set(HMI_CUDNN_RUNTIME_BIN "${_cudnn_bin}" PARENT_SCOPE)
  message(STATUS "native integrated from project sources; runtime native/native.dll")
endfunction()
