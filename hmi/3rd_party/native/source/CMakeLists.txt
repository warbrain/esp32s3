cmake_minimum_required(VERSION 3.20)

project(AudioCpp LANGUAGES C CXX)

set(AUDIOCPP_VERSION "dev" CACHE STRING "audio.cpp version string")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(AUDIOCPP_DEPLOYMENT_BUILD
    "Compile model specs into the runtime for self-contained deployments"
    ON)

# Model specs remain editable runtime data in normal builds. The converter always
# carries the source catalog so the conversion binary is portable. Deployment builds
# additionally compile the catalog into engine_runtime as a fallback for CLI/server.
#
# Each spec becomes one C++ string, and MSVC caps a single string literal at 16380
# single-byte characters (error C2026) before adjacent literals are concatenated --
# qwen3_tts.json is 16.5 KB on its own. So split each spec into adjacent literals,
# cutting at newline boundaries so no chunk splits a UTF-8 sequence and every chunk
# stays plain JSON text.
function(audiocpp_model_spec_literal out json)
    set(AUDIOCPP_SPEC_LIMIT 16000)
    set(AUDIOCPP_SPEC_REST "${json}")
    set(AUDIOCPP_SPEC_LITERALS "")
    while(TRUE)
        string(LENGTH "${AUDIOCPP_SPEC_REST}" AUDIOCPP_SPEC_LEN)
        if(AUDIOCPP_SPEC_LEN LESS_EQUAL AUDIOCPP_SPEC_LIMIT)
            string(APPEND AUDIOCPP_SPEC_LITERALS "R\"AUDIOCPP_SPEC(${AUDIOCPP_SPEC_REST})AUDIOCPP_SPEC\"")
            break()
        endif()
        string(SUBSTRING "${AUDIOCPP_SPEC_REST}" 0 ${AUDIOCPP_SPEC_LIMIT} AUDIOCPP_SPEC_HEAD)
        string(FIND "${AUDIOCPP_SPEC_HEAD}" "\n" AUDIOCPP_SPEC_CUT REVERSE)
        if(AUDIOCPP_SPEC_CUT LESS 1)
            # A spec with no newline in the first 16 KB: cut at the limit.
            set(AUDIOCPP_SPEC_CUT ${AUDIOCPP_SPEC_LIMIT})
        endif()
        string(SUBSTRING "${AUDIOCPP_SPEC_REST}" 0 ${AUDIOCPP_SPEC_CUT} AUDIOCPP_SPEC_CHUNK)
        string(SUBSTRING "${AUDIOCPP_SPEC_REST}" ${AUDIOCPP_SPEC_CUT} -1 AUDIOCPP_SPEC_REST)
        string(APPEND AUDIOCPP_SPEC_LITERALS "R\"AUDIOCPP_SPEC(${AUDIOCPP_SPEC_CHUNK})AUDIOCPP_SPEC\" ")
    endwhile()
    set(${out} "${AUDIOCPP_SPEC_LITERALS}" PARENT_SCOPE)
endfunction()

file(GLOB AUDIOCPP_MODEL_SPEC_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/model_specs/*.json")
set(AUDIOCPP_BUILTIN_MODEL_SPEC_ENTRIES "")
set(AUDIOCPP_CONVERTER_MODEL_SPEC_ENTRIES "")
set(AUDIOCPP_NATIVE_PACKAGE_SPEC_ENTRIES "")
foreach(AUDIOCPP_MODEL_SPEC_FILE IN LISTS AUDIOCPP_MODEL_SPEC_FILES)
    get_filename_component(AUDIOCPP_MODEL_SPEC_FAMILY "${AUDIOCPP_MODEL_SPEC_FILE}" NAME_WE)
    file(READ "${AUDIOCPP_MODEL_SPEC_FILE}" AUDIOCPP_MODEL_SPEC_JSON)
    audiocpp_model_spec_literal(AUDIOCPP_MODEL_SPEC_LITERAL "${AUDIOCPP_MODEL_SPEC_JSON}")
    string(APPEND AUDIOCPP_CONVERTER_MODEL_SPEC_ENTRIES
        "    {\"${AUDIOCPP_MODEL_SPEC_FAMILY}\", ${AUDIOCPP_MODEL_SPEC_LITERAL}},\n")
    string(APPEND AUDIOCPP_NATIVE_PACKAGE_SPEC_ENTRIES
        "    {\"${AUDIOCPP_MODEL_SPEC_FAMILY}\", ${AUDIOCPP_MODEL_SPEC_LITERAL}},\n")
    if (AUDIOCPP_DEPLOYMENT_BUILD)
        string(APPEND AUDIOCPP_BUILTIN_MODEL_SPEC_ENTRIES
            "    {\"${AUDIOCPP_MODEL_SPEC_FAMILY}\", ${AUDIOCPP_MODEL_SPEC_LITERAL}},\n")
    endif()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/model_specs.inc"
    CONTENT "${AUDIOCPP_BUILTIN_MODEL_SPEC_ENTRIES}")
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/gguf_converter_model_specs.inc"
    CONTENT "${AUDIOCPP_CONVERTER_MODEL_SPEC_ENTRIES}")
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/native_package_specs.inc"
    CONTENT "${AUDIOCPP_NATIVE_PACKAGE_SPEC_ENTRIES}")

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if (NOT MSVC)
    set(AUDIOCPP_DEBUG_OPT_FLAGS "-O3 -g")
    set(CMAKE_C_FLAGS_DEBUG "${AUDIOCPP_DEBUG_OPT_FLAGS}" CACHE STRING "Optimized debug C flags" FORCE)
    set(CMAKE_CXX_FLAGS_DEBUG "${AUDIOCPP_DEBUG_OPT_FLAGS}" CACHE STRING "Optimized debug C++ flags" FORCE)
endif()

set(ENGINE_DEFAULT_ENABLE_CUDA OFF)
set(ENGINE_DEFAULT_ENABLE_HIP OFF)
set(ENGINE_DEFAULT_ENABLE_VULKAN OFF)
set(ENGINE_DEFAULT_ENABLE_METAL OFF)
set(ENGINE_DEFAULT_ENABLE_LLAMAFILE ON)
set(ENGINE_DEFAULT_ENABLE_CUDA_GRAPHS ON)
set(ENGINE_DEFAULT_ENABLE_NATIVE_CPU ON)
if (APPLE)
    set(ENGINE_DEFAULT_ENABLE_METAL ON)
endif()
if (DEFINED GGML_CUDA)
    set(ENGINE_DEFAULT_ENABLE_CUDA ${GGML_CUDA})
endif()
if (DEFINED GGML_HIP)
    set(ENGINE_DEFAULT_ENABLE_HIP ${GGML_HIP})
endif()
if (DEFINED GGML_VULKAN)
    set(ENGINE_DEFAULT_ENABLE_VULKAN ${GGML_VULKAN})
endif()
if (DEFINED GGML_METAL)
    set(ENGINE_DEFAULT_ENABLE_METAL ${GGML_METAL})
endif()
if (DEFINED GGML_LLAMAFILE)
    set(ENGINE_DEFAULT_ENABLE_LLAMAFILE ${GGML_LLAMAFILE})
endif()
if (DEFINED GGML_CUDA_GRAPHS)
    set(ENGINE_DEFAULT_ENABLE_CUDA_GRAPHS ${GGML_CUDA_GRAPHS})
endif()

option(ENGINE_ENABLE_CUDA "Build ggml with CUDA backend support" ${ENGINE_DEFAULT_ENABLE_CUDA})
option(ENGINE_ENABLE_HIP "Build ggml with HIP/ROCm backend support" ${ENGINE_DEFAULT_ENABLE_HIP})
option(ENGINE_ENABLE_VULKAN "Build ggml with Vulkan backend support" ${ENGINE_DEFAULT_ENABLE_VULKAN})
option(ENGINE_ENABLE_METAL "Build ggml with Metal backend support" ${ENGINE_DEFAULT_ENABLE_METAL})
option(ENGINE_ENABLE_LLAMAFILE "Build ggml with llamafile SGEMM support" ${ENGINE_DEFAULT_ENABLE_LLAMAFILE})
option(ENGINE_ENABLE_CUDA_GRAPHS "Enable ggml CUDA graphs support" ${ENGINE_DEFAULT_ENABLE_CUDA_GRAPHS})
option(ENGINE_HIP_STRIX_HALO_OPTIMIZATIONS
    "Enable opt-in HIP optimizations validated only for Strix Halo (gfx1151)"
    OFF)
# Rockchip NPU (RK3588 & co): a model-local compute path, not a ggml backend —
# the Qwen3 TTS speech decoder runs a pre-compiled .rknn step graph through
# librknnrt. The engine links the board runtime; the models themselves are
# converted offline on x86 with rknn-toolkit2.
option(ENGINE_ENABLE_RKNN "Build the Rockchip NPU (librknnrt) decoder path" OFF)
option(ENGINE_ENABLE_NEON "Build opt-in NEON library and native model paths" OFF)
set(ENGINE_RKNN_INCLUDE_DIR "" CACHE PATH "Directory containing rknn_api.h (empty = use the system include path)")
set(ENGINE_RKNN_LIBRARY "rknnrt" CACHE STRING "librknnrt to link (path or library name)")
option(ENGINE_ENABLE_NATIVE_CPU "Build ggml CPU kernels with native host ISA flags" ${ENGINE_DEFAULT_ENABLE_NATIVE_CPU})
option(ENGINE_ENABLE_OPENMP "Build host code with OpenMP support" ON)
option(AUDIOCPP_STATIC_ESPEAK "Build and statically link GPL-3.0-or-later eSpeak-ng; keep data separate" OFF)
option(AUDIOCPP_BUILD_NATIVE_MODEL_MANAGER
    "Build native model-manager tools and server download/install support"
    OFF)

option(AUDIOCPP_USE_SYSTEM_OPENSSL
    "Use system OpenSSL for native model management instead of bundled BoringSSL"
    OFF)
if (AUDIOCPP_USE_SYSTEM_OPENSSL AND NOT AUDIOCPP_BUILD_NATIVE_MODEL_MANAGER)
    message(FATAL_ERROR
        "AUDIOCPP_USE_SYSTEM_OPENSSL requires AUDIOCPP_BUILD_NATIVE_MODEL_MANAGER=ON")
endif()
option(ENGINE_ENABLE_CPU_ALL_VARIANTS
    "Build CPU backends as dynamic libraries with per-ISA variants (for Docker/portable builds)"
    OFF)
option(ENGINE_BUILD_EXAMPLES "Build example binaries" OFF)
option(ENGINE_BUILD_TESTS "Build framework unit tests" OFF)
option(ENGINE_BUILD_EXTENDED_TESTS "Build extended non-model tests and probes" OFF)
option(ENGINE_BUILD_MODEL_TESTS "Build model-specific tests and probes" OFF)
option(ENGINE_BUILD_WARMBENCH "Build warmbench helper binaries" OFF)
option(AUDIOCPP_BUILD_C_API "Build the C ABI (include/audiocpp.h) as a shared library" ON)
set(AUDIOCPP_C_API_MODEL_ROOT "" CACHE PATH
    "Models root for the C API model test. Unset leaves the test registered but skipping.")
set(AUDIOCPP_RKNN_TEST_ENGINE "" CACHE PATH
    "A .rknn engine compiled for the build host's SoC, for the RKNN sidecar test. Unset leaves the test registered but skipping.")
set(AUDIOCPP_GGML_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/external/ggml"
    CACHE PATH "ggml source tree to build with AudioCPP")

set(AUDIOCPP_GIT_SHA "unknown")
set(AUDIOCPP_GIT_DATE "unknown")
find_package(Git QUIET)
if (GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE AUDIOCPP_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} show -s --format=%cs HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE AUDIOCPP_GIT_DATE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()

set(AUDIOCPP_BUILD_BACKENDS "cpu")
if (ENGINE_ENABLE_CUDA)
    string(APPEND AUDIOCPP_BUILD_BACKENDS ",cuda")
endif()
if (ENGINE_ENABLE_HIP)
    string(APPEND AUDIOCPP_BUILD_BACKENDS ",hip")
endif()
if (ENGINE_ENABLE_VULKAN)
    string(APPEND AUDIOCPP_BUILD_BACKENDS ",vulkan")
endif()
if (ENGINE_ENABLE_METAL)
    string(APPEND AUDIOCPP_BUILD_BACKENDS ",metal")
endif()
# HIP compiles ggml's CUDA backend sources as HIP code, so the two options drive the
# same ggml backend and cannot coexist: enabling both starts CUDA backend setup and
# HIP backend setup in the same configure, which fails in confusing ways downstream.
if (ENGINE_ENABLE_CUDA AND ENGINE_ENABLE_HIP)
    message(FATAL_ERROR
        "ENGINE_ENABLE_CUDA and ENGINE_ENABLE_HIP are mutually exclusive; "
        "configure with exactly one of them (e.g. -DENGINE_ENABLE_CUDA=OFF -DENGINE_ENABLE_HIP=ON)")
endif()

set(AUDIOCPP_HIP_STRIX_HALO_OPTIMIZATIONS_ACTIVE OFF)
if (ENGINE_HIP_STRIX_HALO_OPTIMIZATIONS)
    if (NOT ENGINE_ENABLE_HIP)
        message(FATAL_ERROR
            "ENGINE_HIP_STRIX_HALO_OPTIMIZATIONS=ON requires ENGINE_ENABLE_HIP=ON")
    endif()

    # Match ggml HIP target precedence: an explicit CMake HIP architecture wins,
    # followed by GPU_TARGETS and then the legacy AMDGPU_TARGETS spelling.
    if (CMAKE_HIP_ARCHITECTURES)
        set(AUDIOCPP_EFFECTIVE_HIP_TARGETS ${CMAKE_HIP_ARCHITECTURES})
        set(AUDIOCPP_EFFECTIVE_HIP_TARGET_SOURCE "CMAKE_HIP_ARCHITECTURES")
    elseif (GPU_TARGETS)
        set(AUDIOCPP_EFFECTIVE_HIP_TARGETS ${GPU_TARGETS})
        set(AUDIOCPP_EFFECTIVE_HIP_TARGET_SOURCE "GPU_TARGETS")
    elseif (AMDGPU_TARGETS)
        set(AUDIOCPP_EFFECTIVE_HIP_TARGETS ${AMDGPU_TARGETS})
        set(AUDIOCPP_EFFECTIVE_HIP_TARGET_SOURCE "AMDGPU_TARGETS")
    else()
        message(FATAL_ERROR
            "ENGINE_HIP_STRIX_HALO_OPTIMIZATIONS=ON requires exactly one explicit HIP target, gfx1151; "
            "set -DGPU_TARGETS=gfx1151 (no project-wide gfx1151 default is provided)")
    endif()

    list(LENGTH AUDIOCPP_EFFECTIVE_HIP_TARGETS AUDIOCPP_EFFECTIVE_HIP_TARGET_COUNT)
    if (NOT AUDIOCPP_EFFECTIVE_HIP_TARGET_COUNT EQUAL 1 OR
        NOT "${AUDIOCPP_EFFECTIVE_HIP_TARGETS}" STREQUAL "gfx1151")
        message(FATAL_ERROR
            "ENGINE_HIP_STRIX_HALO_OPTIMIZATIONS=ON requires the effective HIP target list to be exactly "
            "gfx1151; ${AUDIOCPP_EFFECTIVE_HIP_TARGET_SOURCE}='${AUDIOCPP_EFFECTIVE_HIP_TARGETS}'")
    endif()

    # hipBLASLt regressed decoder compute by roughly 3 seconds on gfx1151.
    # Keep the validated retained-graph path on rocBLAS; the generic HIP build
    # remains unchanged because this option is both target- and backend-gated.
    set(GGML_HIP_HIPBLASLT OFF CACHE BOOL
        "Use hipBLASLt instead of hipBLAS (rocBLAS) for HIP GEMM" FORCE)
    set(AUDIOCPP_HIP_STRIX_HALO_OPTIMIZATIONS_ACTIVE ON)
    message(STATUS "Strix Halo HIP optimizations enabled for gfx1151 with rocBLAS")
endif()

if (ENGINE_ENABLE_CUDA AND NOT ENGINE_ENABLE_HIP)
    # Require at least CUDA 12.0. Older toolkits configure fine but die at the
    # first .cu file. Run this before enable_language(CUDA) because the version
    # is needed for the default arch list below.
    find_package(CUDAToolkit 12.0 REQUIRED)

    # Define default CUDA architectures for every build path.
    # We stay close to llama.cpp/ggml-cuda here, unless there is a reason
    # to deviate. See llama.cpp's ggml-cuda CMakeLists.txt for additional
    # comments.
    # Must be set before enable_language(CUDA), as that call otherwise seeds
    # CMAKE_CUDA_ARCHITECTURES from nvcc's default arch (sm_75 for CUDA 13,
    # sm_52 for CUDA 12), silently making every build without an explicit
    # list single-arch (the seed ignores the local GPU).
    # This can be overridden with -DCMAKE_CUDA_ARCHITECTURES=... or CUDAARCHS;
    # set to "native" to build for the local GPU only (needs CMake >= 3.24).

    # "native" fallback for CMake < 3.24
    if ((CMAKE_CUDA_ARCHITECTURES STREQUAL "native" OR "$ENV{CUDAARCHS}" STREQUAL "native")
        AND CMAKE_VERSION VERSION_LESS "3.24")
        message(STATUS "CMAKE_CUDA_ARCHITECTURES=native requires CMake >= 3.24; "
                       "falling back to the portable default architecture list")
        set(CMAKE_CUDA_ARCHITECTURES "")
    endif()

    # Define default archs
    if ("${CMAKE_CUDA_ARCHITECTURES}" STREQUAL "" AND "$ENV{CUDAARCHS}" STREQUAL "")
        # Remembered so the summary below can tell the difference between "the
        # portable list is what you asked for" and "the portable list is what you
        # got by not asking".
        set(_AUDIOCPP_CUDA_ARCH_DEFAULTED TRUE)
        # XX-virtual = PTX (JIT, forward-compatible); XX-real = SASS (native).
        if (CUDAToolkit_VERSION VERSION_LESS "13")
            # 50/61/70 (Maxwell/Pascal/Volta) unsupported by CUDA 13 toolkits
            list(APPEND CMAKE_CUDA_ARCHITECTURES 50-virtual 61-virtual 70-virtual)
        endif()
        list(APPEND CMAKE_CUDA_ARCHITECTURES 75-real 80-virtual 86-real)
        if (CUDAToolkit_VERSION VERSION_GREATER_EQUAL "11.8")
            list(APPEND CMAKE_CUDA_ARCHITECTURES 89-real 90-virtual)
        endif()
        if (CUDAToolkit_VERSION VERSION_GREATER_EQUAL "12.8")
            # 12Xa, not 12X: Blackwell FP4 tensor cores are arch-specific.
            list(APPEND CMAKE_CUDA_ARCHITECTURES 120a-real)
        endif()
        if (CUDAToolkit_VERSION VERSION_GREATER_EQUAL "12.9")
            list(APPEND CMAKE_CUDA_ARCHITECTURES 121a-real)
        endif()
    endif()

    enable_language(CUDA)

    # Upgrade plain 12X (user value or resolved "native") to 12Xa (Blackwell).
    foreach (_ARCHS_VAR IN ITEMS CMAKE_CUDA_ARCHITECTURES CMAKE_CUDA_ARCHITECTURES_NATIVE)
        set(_FIXED_ARCHS "")
        foreach (_ARCH IN LISTS ${_ARCHS_VAR})
            if (_ARCH MATCHES "^12[0-9](-real|-virtual)?$")
                string(REGEX REPLACE "^(12[0-9])((-real|-virtual)?)$" "\\1a\\2" _FIXED_ARCH "${_ARCH}")
                message(STATUS "Replacing ${_ARCH} in ${_ARCHS_VAR} with ${_FIXED_ARCH}")
                list(APPEND _FIXED_ARCHS "${_FIXED_ARCH}")
            else()
                list(APPEND _FIXED_ARCHS "${_ARCH}")
            endif()
        endforeach()
        set(${_ARCHS_VAR} "${_FIXED_ARCHS}")
    endforeach()

    # Resolve "native" for the log; left as-is when no GPU was detected
    # (nvcc then warns and uses its default arch).
    if (CMAKE_CUDA_ARCHITECTURES STREQUAL "native" AND CMAKE_CUDA_ARCHITECTURES_NATIVE MATCHES "^[0-9]+(a|f)?(-real|-virtual)?(;[0-9]+(a|f)?(-real|-virtual)?|;)*$")
        set(CMAKE_CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES_NATIVE}")
    endif()
    message(STATUS "Using CMAKE_CUDA_ARCHITECTURES=${CMAKE_CUDA_ARCHITECTURES} CMAKE_CUDA_ARCHITECTURES_NATIVE=${CMAKE_CUDA_ARCHITECTURES_NATIVE}")

    # The line above prints both halves of this and interprets neither: how many
    # architectures are about to be compiled, and which one this host can run.
    # The option is documented (README, docs/build/linux.md), but configure is
    # where the choice is actually being made, so say it here too.
    if (_AUDIOCPP_CUDA_ARCH_DEFAULTED AND
        CMAKE_CUDA_ARCHITECTURES_NATIVE MATCHES "^[0-9]")
        list(LENGTH CMAKE_CUDA_ARCHITECTURES _AUDIOCPP_ARCH_COUNT)
        message(STATUS "  portable default: ${_AUDIOCPP_ARCH_COUNT} architectures, so every .cu file is compiled ${_AUDIOCPP_ARCH_COUNT} times")
        message(STATUS "  this host is ${CMAKE_CUDA_ARCHITECTURES_NATIVE}; for a local build roughly 3x faster:")
        message(STATUS "      -DCMAKE_CUDA_ARCHITECTURES=native    (the result then runs on this GPU only)")
        message(STATUS "      scripts/build_linux.sh: --cuda-arch native")
    endif()

    if (NOT MSVC)
        set(CMAKE_CUDA_FLAGS_DEBUG "${AUDIOCPP_DEBUG_OPT_FLAGS}" CACHE STRING "Optimized debug CUDA flags" FORCE)
    endif()
endif()

# ⚠ Outside the CUDA block on purpose: ccache is not a CUDA concern, and a
# CPU-only, Vulkan or HIP build benefits from it just as much. GGML_CCACHE is
# forced OFF above, so ggml's own probe never runs and nothing caches anything
# unless the caller wires a launcher up by hand.
find_program(AUDIOCPP_CCACHE_PROGRAM ccache)
if (AUDIOCPP_CCACHE_PROGRAM AND
    NOT CMAKE_C_COMPILER_LAUNCHER AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    message(STATUS "ccache is installed (${AUDIOCPP_CCACHE_PROGRAM}) and NOT in use. Rebuilds are much faster with:")
    message(STATUS "    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_CUDA_COMPILER_LAUNCHER=ccache")
    message(STATUS "    scripts/build_linux.sh: --ccache")
endif()

if (ENGINE_ENABLE_OPENMP)
    find_package(OpenMP REQUIRED COMPONENTS CXX)
endif()

if (AUDIOCPP_BUILD_C_API)
    # engine_runtime and ggml are static/object libraries here; linking them into
    # a shared libaudiocpp requires position-independent code throughout.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if (ENGINE_ENABLE_CPU_ALL_VARIANTS)
    message(STATUS "CPU all variants mode: enabling shared libraries + backends as dynamic libs")
    set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries" FORCE)
    set(GGML_BACKEND_DL ON CACHE BOOL "Build backends as dynamic libraries" FORCE)
    set(GGML_CPU_ALL_VARIANTS ON CACHE BOOL "Build all CPU backend variants" FORCE)
    set(GGML_NATIVE OFF CACHE BOOL "Build ggml CPU kernels with native host ISA flags" FORCE)
    # Ensure libraries in the same directory as the binary are found at runtime
    set(CMAKE_INSTALL_RPATH "\$ORIGIN")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
else()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
    set(GGML_BACKEND_DL OFF CACHE BOOL "Build backends as dynamic libraries" FORCE)
    set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "Build all CPU backend variants" FORCE)
    set(GGML_NATIVE ${ENGINE_ENABLE_NATIVE_CPU} CACHE BOOL "Build ggml CPU kernels with native host ISA flags" FORCE)
endif()

set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "Do not build upstream examples" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "Do not build upstream tests" FORCE)
set(GGML_ALL_WARNINGS OFF CACHE BOOL "Keep upstream warnings quiet in this sample" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "Do not probe for ccache in this local setup" FORCE)
set(GGML_CUDA ${ENGINE_ENABLE_CUDA} CACHE BOOL "Build ggml with CUDA backend support" FORCE)
set(GGML_HIP ${ENGINE_ENABLE_HIP} CACHE BOOL "Build ggml with HIP backend support" FORCE)
set(GGML_VULKAN ${ENGINE_ENABLE_VULKAN} CACHE BOOL "Build ggml with Vulkan backend support" FORCE)
set(GGML_METAL ${ENGINE_ENABLE_METAL} CACHE BOOL "Build ggml with Metal backend support" FORCE)
set(GGML_LLAMAFILE ${ENGINE_ENABLE_LLAMAFILE} CACHE BOOL "Build ggml with llamafile SGEMM support" FORCE)
set(GGML_CUDA_GRAPHS ${ENGINE_ENABLE_CUDA_GRAPHS} CACHE BOOL "Enable ggml CUDA graphs support" FORCE)
# HIP graphs are controlled by a separate upstream option that defaults to ON;
# keep it in sync so ENGINE_ENABLE_CUDA_GRAPHS=OFF also disables graphs on HIP builds
set(GGML_HIP_GRAPHS ${ENGINE_ENABLE_CUDA_GRAPHS} CACHE BOOL "Enable ggml HIP graphs support" FORCE)

set(SPM_ENABLE_SHARED OFF CACHE BOOL "Build sentencepiece shared libs" FORCE)
set(SPM_BUILD_TEST OFF CACHE BOOL "Do not build sentencepiece tests" FORCE)
set(SPM_ENABLE_TCMALLOC OFF CACHE BOOL "Do not probe for tcmalloc" FORCE)

if (NOT EXISTS "${AUDIOCPP_GGML_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "AUDIOCPP_GGML_SOURCE_DIR does not point to a ggml source tree: ${AUDIOCPP_GGML_SOURCE_DIR}")
endif()
message(STATUS "Using ggml source: ${AUDIOCPP_GGML_SOURCE_DIR}")
add_subdirectory("${AUDIOCPP_GGML_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ggml")
add_subdirectory(external/sentencepiece EXCLUDE_FROM_ALL)

add_compile_options(
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wextra>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wpedantic>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-pedantic-errors>
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->
)

set(AUDIOCPP_MODEL_SET "custom" CACHE STRING "Model composite to build: full, core, or custom")
set_property(CACHE AUDIOCPP_MODEL_SET PROPERTY STRINGS full core custom)
set(AUDIOCPP_MODELS "qwen3_asr;qwen3_tts" CACHE STRING "Comma or semicolon separated model targets for AUDIOCPP_MODEL_SET=custom")
set(AUDIOCPP_LIBYAML_COMPILE_DEFINITIONS
    _POSIX_C_SOURCE=200809L
    YAML_DECLARE_STATIC=1
    YAML_VERSION_MAJOR=0
    YAML_VERSION_MINOR=2
    YAML_VERSION_PATCH=5
    YAML_VERSION_STRING=\"0.2.5\"
)

function(audiocpp_configure_runtime_object target_name)
    target_include_directories(${target_name} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/external/sentencepiece/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/llama_tokenizer
        ${CMAKE_CURRENT_SOURCE_DIR}/external/cJSON
        ${CMAKE_CURRENT_SOURCE_DIR}/external/libyaml/include
    )
    target_compile_definitions(${target_name} PRIVATE ${AUDIOCPP_LIBYAML_COMPILE_DEFINITIONS})
    if (AUDIOCPP_HIP_STRIX_HALO_OPTIMIZATIONS_ACTIVE)
        target_compile_definitions(${target_name} PRIVATE ENGINE_HIP_STRIX_HALO_OPTIMIZATIONS=1)
    endif()
    if (ENGINE_ENABLE_RKNN)
        target_compile_definitions(${target_name} PRIVATE ENGINE_HAVE_RKNN=1)
        if (ENGINE_RKNN_INCLUDE_DIR)
            target_include_directories(${target_name} PRIVATE ${ENGINE_RKNN_INCLUDE_DIR})
        endif()
    endif()
    target_link_libraries(${target_name} PRIVATE ggml sentencepiece)
    if (ENGINE_ENABLE_OPENMP)
        target_link_libraries(${target_name} PRIVATE OpenMP::OpenMP_CXX)
        if (MSVC)
            target_compile_options(${target_name} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/openmp:experimental>)
        endif()
    endif()
endfunction()

function(audiocpp_add_model target_name)
    cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDES;LOADERS;ALIASES;DEPENDS;OBJECT_LIBRARIES" ${ARGN})
    add_library(engine_model_${target_name} OBJECT ${ARG_SOURCES})
    set_target_properties(engine_model_${target_name} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    audiocpp_configure_runtime_object(engine_model_${target_name})
    if (ARG_OBJECT_LIBRARIES)
        target_link_libraries(engine_model_${target_name} PRIVATE ${ARG_OBJECT_LIBRARIES})
    endif()
    if (ENGINE_ENABLE_NEON)
        target_compile_definitions(engine_model_${target_name} PRIVATE ENGINE_HAVE_NEON=1)
    endif()
    set(AUDIOCPP_MODEL_TARGETS ${AUDIOCPP_MODEL_TARGETS} ${target_name} PARENT_SCOPE)
    set(AUDIOCPP_MODEL_${target_name}_INCLUDES "${ARG_INCLUDES}" PARENT_SCOPE)
    set(AUDIOCPP_MODEL_${target_name}_LOADERS "${ARG_LOADERS}" PARENT_SCOPE)
    set(AUDIOCPP_MODEL_${target_name}_DEPENDS "${ARG_DEPENDS}" PARENT_SCOPE)
    set(AUDIOCPP_MODEL_${target_name}_OBJECT_LIBRARIES "${ARG_OBJECT_LIBRARIES}" PARENT_SCOPE)
    set(AUDIOCPP_MODEL_ALIAS_${target_name} ${target_name} PARENT_SCOPE)
    foreach(alias IN LISTS ARG_ALIASES)
        set(AUDIOCPP_MODEL_ALIAS_${alias} ${target_name} PARENT_SCOPE)
    endforeach()
endfunction()

add_library(engine_core OBJECT
    src/framework/assets/resource_bundle.cpp
    src/framework/model_spec/package.cpp
    src/framework/model_spec/options.cpp
    src/framework/model_spec/schema.cpp
    src/framework/assets/tensor_source.cpp
    src/framework/assets/weight_norm_folding.cpp
    src/framework/assets/weight_adapter.cpp
    src/framework/assets/weight_adapter_source.cpp
    src/framework/assets/torch_bin.cpp
    src/framework/core/module.cpp
    src/framework/core/backend.cpp
    src/framework/core/attention_fallback.cpp
    src/framework/core/execution_context.cpp
    src/framework/core/host_memory.cpp
    src/framework/core/load_progress.cpp
    src/framework/runtime/model.cpp
    src/framework/runtime/session.cpp
    src/framework/runtime/task_vocabulary.cpp
    src/framework/runtime/graph_executor.cpp
    src/framework/runtime/kv_cache.cpp
    src/framework/runtime/options.cpp
    src/framework/runtime/session_base.cpp
    src/framework/io/filesystem.cpp
    src/framework/io/config.cpp
    src/framework/io/json.cpp
    src/framework/io/text.cpp
    src/framework/text/chunking.cpp
    src/framework/io/yaml.cpp
    src/framework/io/binary.cpp
    src/framework/io/safetensors.cpp
    src/framework/debug/trace.cpp
    src/framework/debug/profiler.cpp
    src/framework/rknn/engine.cpp
    src/framework/rknn/engine_resources.cpp
    src/framework/decoders/moss_tts_delay/config.cpp
    src/framework/modules/linear_module.cpp
    src/framework/modules/packed_linear_weights.cpp
    src/framework/modules/primitive_modules.cpp
    src/framework/modules/activation_modules.cpp
    src/framework/modules/norm_modules.cpp
    src/framework/modules/lookup_modules.cpp
    src/framework/modules/structural_modules.cpp
    src/framework/modules/conv_modules.cpp
    src/framework/modules/optimizations/fast_conv_modules.cpp
    src/framework/modules/optimizations/fast_projection_modules.cpp
    src/framework/modules/recurrent_modules.cpp
    src/framework/modules/streaming_conv_modules.cpp
    src/framework/modules/feed_forward_modules.cpp
    src/framework/modules/attention/scaled_dot_product_attention.cpp
    src/framework/modules/attention/streaming_frame_attention_cache.cpp
    src/framework/modules/transformers/decoder.cpp
    src/framework/modules/transformers/causal_decoder.cpp
    src/framework/modules/transformers/causal_decoder_runtime.cpp
    src/framework/modules/speech_encoders/qwen3_audio_encoder_runtime.cpp
    src/framework/modules/conditioning_modules.cpp
    src/framework/modules/positional_modules.cpp
    src/framework/modules/optimizations/fast_kv_modules.cpp
    src/framework/tokenizers/llama_bpe.cpp
    external/llama_tokenizer/bpe-core.cpp
    external/llama_tokenizer/unicode.cpp
    external/llama_tokenizer/unicode-data.cpp
    src/framework/audio/fft.cpp
    src/framework/audio/probability_segmenter.cpp
    src/framework/audio/chunking.cpp
    src/framework/audio/conversion.cpp
    src/framework/audio/utilities/deepfilternet2.cpp
    src/framework/audio/dsp.cpp
    src/framework/audio/mel_spectrogram_frontend.cpp
    src/framework/audio/utilities/flashsr.cpp
    src/framework/audio/utilities/gtcrn.cpp
    src/framework/audio/utilities/hgtcrn.cpp
    src/framework/audio/resampling.cpp
    src/framework/audio/utilities/rnnoise.cpp
    src/framework/audio/utilities/utility_api.cpp
    src/framework/audio/waveform_ops.cpp
    src/framework/audio/wav_reader.cpp
    src/framework/audio/wav_writer.cpp
    src/framework/audio/utilities/zipenhancer.cpp
    src/framework/sampling/greedy_decode.cpp
    src/framework/sampling/hf_sampler.cpp
    src/framework/sampling/torch_random.cpp
)
audiocpp_configure_runtime_object(engine_core)

# NVTX uses the installed CUDA headers and adds no runtime/link dependency.
if (ENGINE_ENABLE_CUDA)
    foreach(_native_nvtx_include IN LISTS CUDAToolkit_INCLUDE_DIRS)
        if (EXISTS "${_native_nvtx_include}/nvtx3/nvToolsExt.h")
            target_include_directories(engine_core PRIVATE "${_native_nvtx_include}")
            target_compile_definitions(engine_core PRIVATE ENGINE_HAS_NVTX=1)
            break()
        endif()
    endforeach()
endif()

# These two small built-in model paths are kept outside the selectable composite list for now.
add_library(engine_model_silero_vad OBJECT
    src/models/silero_vad/assets.cpp
    src/models/silero_vad/runtime.cpp
    src/models/silero_vad/session.cpp
)
set_target_properties(engine_model_silero_vad PROPERTIES EXCLUDE_FROM_ALL TRUE)
audiocpp_configure_runtime_object(engine_model_silero_vad)

add_library(engine_model_marblenet_vad OBJECT
    src/models/marblenet_vad/assets.cpp
    src/models/marblenet_vad/runtime.cpp
    src/models/marblenet_vad/session.cpp
)
set_target_properties(engine_model_marblenet_vad PROPERTIES EXCLUDE_FROM_ALL TRUE)
audiocpp_configure_runtime_object(engine_model_marblenet_vad)

audiocpp_add_model(qwen3_asr
    SOURCES
        src/models/qwen3_asr/assets.cpp
        src/models/qwen3_asr/tokenizer_text.cpp
        src/models/qwen3_asr/frontend_whisper.cpp
        src/models/qwen3_asr/thinker.cpp
        src/models/qwen3_asr/prompt_asr.cpp
        src/models/qwen3_asr/postprocess.cpp
        src/models/qwen3_asr/session.cpp
        src/models/qwen3_asr/loader.cpp
    INCLUDES
        engine/models/qwen3_asr/loader.h
    LOADERS
        engine::models::qwen3_asr::make_qwen3_asr_loader
    DEPENDS
        qwen3_forced_aligner
)

audiocpp_add_model(qwen3_tts
    SOURCES
        src/models/qwen3_tts/assets.cpp
        src/models/qwen3_tts/tokenizer_text.cpp
        src/models/qwen3_tts/tokenizer_speech_encoder.cpp
        src/models/qwen3_tts/tokenizer_speech_decoder.cpp
        src/models/qwen3_tts/rknn_speech_decoder.cpp
        src/models/qwen3_tts/speaker_encoder.cpp
        src/models/qwen3_tts/talker.cpp
        src/models/qwen3_tts/prompt_tts_voice_clone.cpp
        src/models/qwen3_tts/prompt_tts_voice_design.cpp
        src/models/qwen3_tts/prompt_tts_custom_voice.cpp
        src/models/qwen3_tts/session.cpp
        src/models/qwen3_tts/loader.cpp
    INCLUDES
        engine/models/qwen3_tts/loader.h
    LOADERS
        engine::models::qwen3_tts::make_qwen3_tts_loader
)

audiocpp_add_model(qwen3_forced_aligner
    SOURCES
        src/models/qwen3_forced_aligner/processor.cpp
        src/models/qwen3_forced_aligner/session.cpp
        src/models/qwen3_forced_aligner/loader.cpp
    INCLUDES
        engine/models/qwen3_forced_aligner/loader.h
    LOADERS
        engine::models::qwen3_forced_aligner::make_qwen3_forced_aligner_loader
    DEPENDS
        qwen3_asr
)

set(AUDIOCPP_ENABLED_MODELS "")
if (AUDIOCPP_MODEL_SET STREQUAL "full")
    set(AUDIOCPP_ENABLED_MODELS ${AUDIOCPP_MODEL_TARGETS})
elseif (AUDIOCPP_MODEL_SET STREQUAL "core")
    set(AUDIOCPP_ENABLED_MODELS "")
elseif (AUDIOCPP_MODEL_SET STREQUAL "custom")
    string(REPLACE "," ";" AUDIOCPP_REQUESTED_MODELS "${AUDIOCPP_MODELS}")
    foreach(requested_model IN LISTS AUDIOCPP_REQUESTED_MODELS)
        string(STRIP "${requested_model}" requested_model)
        if (requested_model STREQUAL "")
            continue()
        endif()
        if (DEFINED AUDIOCPP_MODEL_ALIAS_${requested_model})
            list(APPEND AUDIOCPP_ENABLED_MODELS ${AUDIOCPP_MODEL_ALIAS_${requested_model}})
        else()
            message(FATAL_ERROR "Unknown AUDIOCPP_MODELS entry: ${requested_model}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES AUDIOCPP_ENABLED_MODELS)
else()
    message(FATAL_ERROR "AUDIOCPP_MODEL_SET must be full, core, or custom")
endif()

set(AUDIOCPP_REGISTRY_INCLUDE_CONTENT "")
set(AUDIOCPP_REGISTRY_LOADER_CONTENT "")
foreach(model_name IN LISTS AUDIOCPP_ENABLED_MODELS)
    foreach(include_name IN LISTS AUDIOCPP_MODEL_${model_name}_INCLUDES)
        string(APPEND AUDIOCPP_REGISTRY_INCLUDE_CONTENT "#include \"${include_name}\"\n")
    endforeach()
    foreach(loader_name IN LISTS AUDIOCPP_MODEL_${model_name}_LOADERS)
        string(APPEND AUDIOCPP_REGISTRY_LOADER_CONTENT "        ${loader_name}(),\n")
    endforeach()
endforeach()

set(AUDIOCPP_LINKED_MODELS ${AUDIOCPP_ENABLED_MODELS})
set(AUDIOCPP_DEPENDENCY_SCAN_INDEX 0)
list(LENGTH AUDIOCPP_LINKED_MODELS AUDIOCPP_LINKED_MODEL_COUNT)
while(AUDIOCPP_DEPENDENCY_SCAN_INDEX LESS AUDIOCPP_LINKED_MODEL_COUNT)
    list(GET AUDIOCPP_LINKED_MODELS ${AUDIOCPP_DEPENDENCY_SCAN_INDEX} model_name)
    foreach(dependency IN LISTS AUDIOCPP_MODEL_${model_name}_DEPENDS)
        if (NOT dependency IN_LIST AUDIOCPP_MODEL_TARGETS)
            message(FATAL_ERROR "Unknown composite dependency for ${model_name}: ${dependency}")
        endif()
        if (NOT dependency IN_LIST AUDIOCPP_LINKED_MODELS)
            list(APPEND AUDIOCPP_LINKED_MODELS ${dependency})
        endif()
    endforeach()
    math(EXPR AUDIOCPP_DEPENDENCY_SCAN_INDEX "${AUDIOCPP_DEPENDENCY_SCAN_INDEX} + 1")
    list(LENGTH AUDIOCPP_LINKED_MODELS AUDIOCPP_LINKED_MODEL_COUNT)
endwhile()
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/model_registry_includes.inc"
    CONTENT "${AUDIOCPP_REGISTRY_INCLUDE_CONTENT}")
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/model_registry_loaders.inc"
    CONTENT "${AUDIOCPP_REGISTRY_LOADER_CONTENT}")

set(AUDIOCPP_RUNTIME_OBJECTS
    $<TARGET_OBJECTS:engine_core>
    $<TARGET_OBJECTS:engine_model_silero_vad>
    $<TARGET_OBJECTS:engine_model_marblenet_vad>
)
if (ENGINE_ENABLE_NEON)
    list(APPEND AUDIOCPP_RUNTIME_OBJECTS $<TARGET_OBJECTS:engine_neon>)
endif()
set(AUDIOCPP_MODEL_OBJECT_LIBRARIES "")
foreach(model_name IN LISTS AUDIOCPP_LINKED_MODELS)
    list(APPEND AUDIOCPP_RUNTIME_OBJECTS $<TARGET_OBJECTS:engine_model_${model_name}>)
    list(APPEND AUDIOCPP_MODEL_OBJECT_LIBRARIES ${AUDIOCPP_MODEL_${model_name}_OBJECT_LIBRARIES})
endforeach()
list(REMOVE_DUPLICATES AUDIOCPP_MODEL_OBJECT_LIBRARIES)
foreach(object_library IN LISTS AUDIOCPP_MODEL_OBJECT_LIBRARIES)
    list(APPEND AUDIOCPP_RUNTIME_OBJECTS $<TARGET_OBJECTS:${object_library}>)
endforeach()

message(STATUS "audio.cpp model composite: ${AUDIOCPP_MODEL_SET} selected [${AUDIOCPP_ENABLED_MODELS}] linked [${AUDIOCPP_LINKED_MODELS}]")
add_library(engine_runtime STATIC
    src/framework/runtime/registry.cpp
    ${AUDIOCPP_RUNTIME_OBJECTS}
)
if (ENGINE_ENABLE_NEON)
    target_compile_definitions(engine_runtime PUBLIC ENGINE_HAVE_NEON=1)
endif()

add_library(cjson_vendor STATIC
    external/cJSON/cJSON.c
)

# cJSON annotates every CJSON_PUBLIC function with __declspec(dllexport) on
# Windows unless told otherwise (external/cJSON/cJSON.h:59-66 defaults
# CJSON_EXPORT_SYMBOLS on). Those annotations live in the object files, so any
# DLL that absorbs this static library re-exports all 78 of them -- dllexport is
# additive, it says "also export this", never "export only this". Measured: a
# shared build exported 147 symbols where 69 were intended.
#
# No DLL is built from this tree by default, so this is latent rather than
# broken today; AUDIOCPP_BUILD_C_API is the first target to expose it.
# CJSON_HIDE_SYMBOLS is cJSON's own mechanism for exactly this case and selects
# a plain declaration with no storage class. PUBLIC rather than PRIVATE so every
# translation unit that includes cJSON.h agrees on the linkage.
target_compile_definitions(cjson_vendor PUBLIC CJSON_HIDE_SYMBOLS)

target_include_directories(cjson_vendor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/external/cJSON
)

# Native package management is deliberately opt-in. Normal CLI and server
# builds do not configure an HTTP/TLS dependency. When enabled, package specs
# remain embedded so the standalone manager and managed WebUI do not require a
# source checkout or an external model_specs directory.
add_library(yaml_vendor STATIC
    external/libyaml/src/api.c
    external/libyaml/src/reader.c
    external/libyaml/src/scanner.c
    external/libyaml/src/parser.c
    external/libyaml/src/loader.c
)

target_include_directories(yaml_vendor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/external/libyaml/include
)

target_compile_definitions(yaml_vendor PUBLIC ${AUDIOCPP_LIBYAML_COMPILE_DEFINITIONS})

target_include_directories(engine_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(engine_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_include_directories(engine_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/sentencepiece/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external/llama_tokenizer
)

target_link_libraries(engine_runtime PUBLIC ggml)
find_package(Threads REQUIRED)
target_link_libraries(engine_runtime PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(engine_runtime PRIVATE sentencepiece cjson_vendor yaml_vendor)
if(AUDIOCPP_STATIC_ESPEAK)
    include(tools/cmake/espeak_static.cmake)
endif()
if (AUDIOCPP_HIP_STRIX_HALO_OPTIMIZATIONS_ACTIVE)
    target_compile_definitions(engine_runtime PRIVATE ENGINE_HIP_STRIX_HALO_OPTIMIZATIONS=1)
endif()
if (ENGINE_ENABLE_RKNN)
    target_compile_definitions(engine_runtime PRIVATE ENGINE_HAVE_RKNN=1)
    if (ENGINE_RKNN_INCLUDE_DIR)
        target_include_directories(engine_runtime PUBLIC ${ENGINE_RKNN_INCLUDE_DIR})
    endif()
    target_link_libraries(engine_runtime PUBLIC ${ENGINE_RKNN_LIBRARY})
endif()
if (ENGINE_ENABLE_HIP)
    # ggml-hip publicly defines GGML_USE_CUDA for consumers, so engine code
    # needs this marker to tell a real CUDA build apart from a HIP build.
    target_compile_definitions(engine_core PRIVATE ENGINE_GGML_HIP_BACKEND=1)
endif()

# Fish Fast-AR emits a 4096-entry logits tensor for each acoustic codebook.
# On Linux HIP, a small device-side exact top-k helper keeps only the compact
# sampling candidates crossing back to the host. Built as a proper HIP target
# so CMake tracks its HIP runtime dependency; other backends stay unchanged.
if (ENGINE_ENABLE_OPENMP)
    target_link_libraries(engine_runtime PRIVATE OpenMP::OpenMP_CXX)
    if (MSVC)
        # MSVC's default /openmp implements only OpenMP 2.0 and rejects the
        # '#pragma omp simd' directives in longformer_attention.cpp (error C7660).
        # /openmp:experimental enables the OpenMP 4.0 SIMD support; it overrides the
        # /openmp added by OpenMP::OpenMP_CXX above (harmless D9025 override notice).
        # Guard on CXX: nvcc does not forward a bare /openmp:experimental to the host
        # compiler and instead treats it as a second input file, which is fatal.
        target_compile_options(engine_runtime PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/openmp:experimental>)
    endif()
endif()

if (MSVC)
    target_compile_options(engine_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
    target_compile_options(engine_runtime PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

if (ENGINE_ENABLE_CUDA AND NOT ENGINE_ENABLE_HIP)
    target_sources(engine_runtime PRIVATE
        src/framework/audio/istft_cuda_runtime.cu
        src/framework/sampling/torch_random_cuda_runtime.cu
    )
    set_source_files_properties(
        src/framework/audio/istft_cuda_runtime.cu
        src/framework/sampling/torch_random_cuda_runtime.cu
        PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
    )
    target_compile_definitions(engine_core PRIVATE ENGINE_HAS_CUDA_ISTFT=1 ENGINE_HAS_CUDA_TORCH_RANDOM=1)
    target_compile_definitions(engine_runtime PUBLIC ENGINE_HAS_CUDA_ISTFT=1 ENGINE_HAS_CUDA_TORCH_RANDOM=1)
    target_link_libraries(engine_runtime PRIVATE CUDA::cudart CUDA::cufft)
endif()


if (AUDIOCPP_BUILD_C_API)
    add_library(audiocpp SHARED src/capi/audiocpp.cpp src/capi/audio_frontend.cpp)
    target_link_libraries(audiocpp PRIVATE engine_runtime ggml)
    target_include_directories(audiocpp PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_definitions(audiocpp
        PRIVATE
            AUDIOCPP_BUILD_DLL
            AUDIOCPP_VERSION_STRING="${AUDIOCPP_VERSION}"
        # Anything linking this gets __declspec(dllimport) on Windows without
        # having to know to ask for it. No effect elsewhere.
        INTERFACE
            AUDIOCPP_USE_DLL
    )
    set_target_properties(audiocpp PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        VERSION 0.1.0
        SOVERSION 0
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    if (APPLE)
        set_target_properties(audiocpp PROPERTIES
            INSTALL_RPATH "@loader_path" BUILD_WITH_INSTALL_RPATH ON)
    elseif (UNIX)
        set_target_properties(audiocpp PROPERTIES
            INSTALL_RPATH "$ORIGIN" BUILD_WITH_INSTALL_RPATH ON)
    endif()
    # The visibility presets above only reach this target's own sources. The
    # static libraries linked in (ggml, cJSON, sentencepiece, ...) are built
    # without -fvisibility=hidden, so without an explicit export list they are
    # re-exported from libaudiocpp -- measured at 2767 dynamic symbols where the
    # header declares 69 -- and can collide with whatever the host application
    # has already loaded. Restrict the surface to audiocpp_*.
    #
    # Windows has no equivalent of these allowlists: __declspec(dllexport) is
    # additive, so a DLL's surface is whatever its own annotations plus its
    # dependencies' annotations add up to. Exclusivity there comes from making
    # sure no dependency annotates its symbols, which is why cjson_vendor is
    # built with CJSON_HIDE_SYMBOLS above.
    if (APPLE)
        target_link_options(audiocpp PRIVATE
            "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/capi/audiocpp.symbols")
        set_property(TARGET audiocpp APPEND PROPERTY
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/capi/audiocpp.symbols")
    elseif (NOT WIN32)
        target_link_options(audiocpp PRIVATE
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/capi/audiocpp.map"
            "-Wl,--exclude-libs,ALL")
        set_property(TARGET audiocpp APPEND PROPERTY
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/capi/audiocpp.map")
    endif()
    if (ENGINE_ENABLE_OPENMP)
        target_link_libraries(audiocpp PRIVATE OpenMP::OpenMP_CXX)
    endif()
    if(AUDIOCPP_STATIC_ESPEAK)
        audiocpp_stage_espeak(audiocpp)
    endif()


    if(MSVC)
        target_link_options(audiocpp PRIVATE "/MAP:${CMAKE_BINARY_DIR}/audiocpp-link.map" /INCREMENTAL:NO)
    endif()
endif()

# SDK-only extraction: optional non-SDK targets and alternative platform branches are excluded.
if(AUDIOCPP_BUILD_NATIVE_MODEL_MANAGER OR AUDIOCPP_STATIC_ESPEAK OR ENGINE_ENABLE_HIP OR ENGINE_ENABLE_NEON OR ENGINE_ENABLE_RKNN)
    message(FATAL_ERROR "This source subset supports CPU, CUDA and Vulkan SDK builds only.")
endif()
