cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectPaths.cmake")

if(NOT TEST_ROOT)
  message(FATAL_ERROR "TEST_ROOT is required")
endif()
set(_project "${TEST_ROOT}/project")
file(MAKE_DIRECTORY "${_project}/3rd_party/sdk" "${TEST_ROOT}/outside-sdk")
set(PROJECT_SOURCE_DIR "${_project}")
hmi_require_project_path("${_project}/3rd_party/sdk" "fixture SDK")
hmi_require_project_path("3rd_party/sdk" "relative fixture SDK")
hmi_require_project_path("${_project}/build/future-output" "future output")
hmi_require_build_path("${_project}/build" "fixture build")
hmi_require_build_path("build/ninja/future-output" "relative nested build")
if(WIN32)
  string(TOUPPER "${_project}/build/ninja/future-output" _upper_build)
  hmi_require_build_path("${_upper_build}" "case-insensitive nested build")
endif()
if(EXISTS "${_project}/build/ninja/future-output")
  message(FATAL_ERROR "Build path validation must not create output directories")
endif()

# Relative output overrides use the binary directory, and supported config
# placeholders are checked without creating any library or debug directories.
function(check_local_output_overrides)
  set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Custom")
  set(CMAKE_BUILD_TYPE "CustomSingle")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "lib/$<CONFIG>")
  set(CMAKE_PDB_OUTPUT_DIRECTORY_RELEASE "debug/$<CONFIG>")
  hmi_require_build_outputs("${_project}/build/ninja" "${_project}")
endfunction()
check_local_output_overrides()
if(EXISTS "${_project}/build/ninja/lib" OR EXISTS "${_project}/build/ninja/debug")
  message(FATAL_ERROR "Output override validation must not create directories")
endif()

function(check_rejected_output_override variable value expected_error)
  set(_check "${TEST_ROOT}/reject-output-override.cmake")
  file(WRITE "${_check}"
    "include([==[${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../cmake/ProjectPaths.cmake]==])\nset(${variable} [==[${value}]==])\nhmi_require_build_outputs([==[${_project}/build/ninja]==] [==[${_project}]==])\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_check}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _error)
  string(REGEX REPLACE "[ \t\r\n]+" " " _diagnostic "${_out}${_error}")
  if(_result EQUAL 0 OR NOT _diagnostic MATCHES "${expected_error}")
    message(FATAL_ERROR "Unsafe ${variable} override was not rejected: ${_out}${_error}")
  endif()
endfunction()
check_rejected_output_override(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${_project}/src"
  "must stay inside the project build directory")
check_rejected_output_override(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${_project}/src"
  "must stay inside the project build directory")
check_rejected_output_override(CMAKE_RUNTIME_OUTPUT_DIRECTORY_ASAN-RELEASE "${_project}/src"
  "must stay inside the project build directory")
check_rejected_output_override(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${_project}/src"
  "must stay inside the project build directory")
check_rejected_output_override(CMAKE_LIBRARY_OUTPUT_DIRECTORY "$<IF:$<CONFIG:Debug>,lib,other>"
  "unsupported generator expression")

# An existing project-local vendor cache entry migrates without changing tuning.
set(HMI_AUDIOCPP_SOURCE_ROOT "${_project}/3rd_party/audiocpp/source" CACHE PATH "fixture legacy source" FORCE)
set(HMI_AUDIOCPP_SDK_OPENMP OFF CACHE BOOL "fixture tuning" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AudioSource.cmake")
if(NOT HMI_AUDIOCPP_SOURCE_ROOT STREQUAL "${_project}/3rd_party/native/source" OR HMI_AUDIOCPP_SDK_OPENMP)
  message(FATAL_ERROR "Native source cache migration changed tuning or kept the old path")
endif()
# Explicit roots work before project(), when PROJECT_SOURCE_DIR is not set.
unset(PROJECT_SOURCE_DIR)
hmi_require_project_path("${_project}/build/future-output" "pre-project output" "${_project}")
hmi_require_build_path("${_project}/build/future-output" "pre-project build" "${_project}")
set(PROJECT_SOURCE_DIR "${_project}")

# Only the build tree owns generated files. A shared project prefix, a sibling
# named build-copy and a traversal back into sources must not be sufficient.
foreach(_path "${_project}" "${_project}/src" "${_project}/3rd_party/sdk"
    "${_project}/build-copy/future-output" "${_project}/build/../src/future-output")
  set(_existed FALSE)
  if(EXISTS "${_path}")
    set(_existed TRUE)
  endif()
  set(_check "${TEST_ROOT}/reject-non-build.cmake")
  file(WRITE "${_check}"
    "set(PROJECT_SOURCE_DIR [==[${_project}]==])\ninclude([==[${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectPaths.cmake]==])\nhmi_require_build_path([==[${_path}]==] [==[fixture output]==])\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_check}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _error)
  if(_result EQUAL 0 OR NOT "${_out}${_error}" MATCHES "must stay inside the project build directory")
    message(FATAL_ERROR "Non-build output path was not rejected: ${_path}: ${_out}${_error}")
  endif()
  if(NOT _existed AND EXISTS "${_path}")
    message(FATAL_ERROR "Rejected build path was created: ${_path}")
  endif()
endforeach()

foreach(_path "${TEST_ROOT}/outside-sdk" "${_project}/../outside-sdk" "${TEST_ROOT}/project-copy/sdk")
  set(_check "${TEST_ROOT}/reject-external.cmake")
  file(WRITE "${_check}"
    "set(PROJECT_SOURCE_DIR [==[${_project}]==])\ninclude([==[${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectPaths.cmake]==])\nhmi_require_project_path([==[${_path}]==] [==[fixture SDK]==])\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_check}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _error)
  if(_result EQUAL 0 OR NOT "${_out}${_error}" MATCHES "must stay inside the project")
    message(FATAL_ERROR "External source path was not rejected: ${_path}: ${_out}${_error}")
  endif()
endforeach()

set(_check "${TEST_ROOT}/reject-external-build.cmake")
file(WRITE "${_check}"
  "set(PROJECT_SOURCE_DIR [==[${_project}]==])\ninclude([==[${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectPaths.cmake]==])\nhmi_require_build_path([==[${TEST_ROOT}/outside-sdk/future-output]==] [==[fixture output]==])\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_check}"
  RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _error)
if(_result EQUAL 0 OR NOT "${_out}${_error}" MATCHES "must stay inside the project:")
  message(FATAL_ERROR "External build path did not preserve the project boundary error: ${_out}${_error}")
endif()

# A direct compiler-cache include must reject outside storage even when the
# build script is bypassed. The rejected path is never created or populated.
get_filename_component(_owner_project "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_outside_cache "${_owner_project}/../hmi-external-cache-rejected")
set(_check "${TEST_ROOT}/reject-external-cache.cmake")
file(WRITE "${_check}"
  "set(HMI_CCACHE_EXECUTABLE [==[]==])\nset(HMI_USE_CCACHE OFF CACHE BOOL [==[]==])\nset(HMI_CCACHE_DIR [==[${_outside_cache}]==] CACHE PATH [==[]==])\ninclude([==[${CMAKE_CURRENT_LIST_DIR}/../cmake/CompilerCache.cmake]==] NO_POLICY_SCOPE)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_check}"
  RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _error)
if(_result EQUAL 0 OR NOT "${_out}${_error}" MATCHES "Compiler cache must stay inside the project")
  message(FATAL_ERROR "External compiler cache was not rejected: ${_out}${_error}")
endif()

set(_source_cache "${_owner_project}/.hmi-source-cache-rejected")
set(_cache_existed FALSE)
if(EXISTS "${_source_cache}")
  set(_cache_existed TRUE)
endif()
set(_check "${TEST_ROOT}/reject-source-cache.cmake")
file(WRITE "${_check}"
  "set(HMI_CCACHE_EXECUTABLE [==[]==])\nset(HMI_USE_CCACHE OFF CACHE BOOL [==[]==])\nset(HMI_CCACHE_DIR [==[${_source_cache}]==] CACHE PATH [==[]==])\ninclude([==[${CMAKE_CURRENT_LIST_DIR}/../cmake/CompilerCache.cmake]==] NO_POLICY_SCOPE)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_check}"
  RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _error)
if(_result EQUAL 0 OR NOT "${_out}${_error}" MATCHES "Compiler cache must stay inside the project build directory")
  message(FATAL_ERROR "Source-tree compiler cache was not rejected: ${_out}${_error}")
endif()
if(NOT _cache_existed AND EXISTS "${_source_cache}")
  message(FATAL_ERROR "Rejected compiler cache was created in the source tree")
endif()

# If symbolic links are available, also check not-yet-created files under one.
set(_link "${_project}/sdk-link")
file(CREATE_LINK "${TEST_ROOT}/outside-sdk" "${_link}" SYMBOLIC RESULT _linked)
if(_linked STREQUAL "0")
  set(_check "${TEST_ROOT}/reject-link.cmake")
  file(WRITE "${_check}"
    "set(PROJECT_SOURCE_DIR [==[${_project}]==])\ninclude([==[${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectPaths.cmake]==])\nhmi_require_project_path([==[${_link}/future/file]==] [==[fixture SDK]==])\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_check}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _error)
  file(REMOVE "${_link}")
  if(_result EQUAL 0 OR NOT "${_out}${_error}" MATCHES "must stay inside the project")
    message(FATAL_ERROR "Redirected future output was not rejected: ${_out}${_error}")
  endif()
else()
  message(STATUS "Symbolic link creation unavailable; link-specific path fixture skipped")
endif()

# A link can stay in the project and still escape the build tree. Check both
# a nested redirect and a build/ root redirected into a sibling source folder.
file(MAKE_DIRECTORY "${_project}/build" "${_project}/src")
set(_link "${_project}/build/source-link")
file(CREATE_LINK "${_project}/src" "${_link}" SYMBOLIC RESULT _linked)
if(_linked STREQUAL "0")
  set(_check "${TEST_ROOT}/reject-build-link.cmake")
  file(WRITE "${_check}"
    "set(PROJECT_SOURCE_DIR [==[${_project}]==])\ninclude([==[${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectPaths.cmake]==])\nhmi_require_build_path([==[${_link}/future-output]==] [==[fixture output]==])\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_check}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _error)
  file(REMOVE "${_link}")
  if(_result EQUAL 0 OR NOT "${_out}${_error}" MATCHES "must stay inside the project build directory")
    message(FATAL_ERROR "Project-local redirect outside build was not rejected: ${_out}${_error}")
  endif()
else()
  message(STATUS "Symbolic link creation unavailable; nested build redirect fixture skipped")
endif()
set(_redirected_project "${TEST_ROOT}/redirected-project")
file(MAKE_DIRECTORY "${_redirected_project}/src")
set(_link "${_redirected_project}/build")
file(CREATE_LINK "${_redirected_project}/src" "${_link}" SYMBOLIC RESULT _linked)
if(_linked STREQUAL "0")
  set(_check "${TEST_ROOT}/reject-build-root-link.cmake")
  file(WRITE "${_check}"
    "set(PROJECT_SOURCE_DIR [==[${_redirected_project}]==])\ninclude([==[${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectPaths.cmake]==])\nhmi_require_build_path([==[${_link}/future-output]==] [==[fixture output]==])\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_check}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _error)
  file(REMOVE "${_link}")
  if(_result EQUAL 0 OR NOT "${_out}${_error}" MATCHES "must stay inside the project build directory")
    message(FATAL_ERROR "Redirected build root was not rejected: ${_out}${_error}")
  endif()
else()
  message(STATUS "Symbolic link creation unavailable; build root redirect fixture skipped")
endif()
message(STATUS "Project source and build-only output boundary checks passed")
