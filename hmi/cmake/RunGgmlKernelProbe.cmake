cmake_minimum_required(VERSION 3.24)

foreach(_required IN ITEMS PROBE_EXECUTABLE PROBE_MODE PROBE_BACKEND PROBE_OPS PROBE_TIMEOUT PROBE_OUTPUT_DIR)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "Missing ggml diagnostic argument: ${_required}")
  endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/ProjectPaths.cmake")
get_filename_component(_owner_project "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
cmake_path(ABSOLUTE_PATH PROBE_OUTPUT_DIR)
hmi_require_build_path("${PROBE_OUTPUT_DIR}" "ggml diagnostic output" "${_owner_project}")
if(NOT PROBE_MODE STREQUAL "support" AND NOT PROBE_MODE STREQUAL "perf")
  message(FATAL_ERROR "PROBE_MODE must be support or perf.")
endif()
if(NOT PROBE_TIMEOUT MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "PROBE_TIMEOUT must be a positive integer.")
endif()
if(NOT EXISTS "${PROBE_EXECUTABLE}")
  message(FATAL_ERROR "ggml diagnostic executable is missing: ${PROBE_EXECUTABLE}")
endif()

file(MAKE_DIRECTORY "${PROBE_OUTPUT_DIR}")
string(REGEX REPLACE "[^A-Za-z0-9_.-]" "_" _backend_file "${PROBE_BACKEND}")
set(_stem "${PROBE_OUTPUT_DIR}/${_backend_file}-${PROBE_MODE}")
if(PROBE_MODE STREQUAL "support")
  set(_format csv)
else()
  # The upstream CSV printer omits time_us/flops/bandwidth/n_runs. SQL retains
  # every original result field, without modifying the upstream benchmark.
  set(_format sql)
endif()
set(_output "${_stem}.${_format}")
set(_stderr "${_stem}.stderr.log")
set(_result "${_stem}.result.txt")
get_filename_component(_working_directory "${PROBE_EXECUTABLE}" DIRECTORY)

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=GGML_BACKEND_PATH
    "${PROBE_EXECUTABLE}" "${PROBE_MODE}" -b "${PROBE_BACKEND}" -o "${PROBE_OPS}" -p "${PROBE_PARAMS}" --output "${_format}"
  WORKING_DIRECTORY "${_working_directory}"
  TIMEOUT "${PROBE_TIMEOUT}"
  RESULT_VARIABLE _exit
  OUTPUT_FILE "${_output}"
  ERROR_FILE "${_stderr}")
file(WRITE "${_result}" "mode=${PROBE_MODE}\nbackend=${PROBE_BACKEND}\nops=${PROBE_OPS}\nparams=${PROBE_PARAMS}\nexit=${_exit}\noutput=${_output}\nstderr=${_stderr}\n")

if(NOT "${_exit}" STREQUAL "0")
  file(APPEND "${_result}" "validation=failed_process\n")
  message(FATAL_ERROR "ggml ${PROBE_MODE} failed (${_exit}). Output: ${_output}; errors: ${_stderr}")
endif()

file(READ "${_output}" _stdout_text)
file(READ "${_stderr}" _stderr_text)
if("${_stdout_text}\n${_stderr_text}" MATCHES "[Ff]ailed to allocate tensors|ggml_backend_graph_compute failed")
  # Upstream perf dispatch currently ignores eval_perf(false); exit 0 alone
  # cannot establish that a kernel actually completed.
  file(APPEND "${_result}" "validation=failed_kernel_execution\n")
  message(FATAL_ERROR "ggml reported a kernel execution/allocation failure. Output: ${_output}; errors: ${_stderr}")
endif()

file(STRINGS "${_output}" _lines ENCODING UTF-8)
set(_rows 0)
set(_measured 0)
set(_unsupported 0)
set(_invalid 0)
foreach(_line IN LISTS _lines)
  if(PROBE_MODE STREQUAL "support")
    # Field order in this pinned upstream CSV is backend_name, op_name,
    # op_params, test_mode, supported, error_message, backend_reg_name.
    if(_line MATCHES "^\"[^\"]+\",\"[^\"]+\",.*,[ ]*\"support\",\"([01])\",.*$")
      math(EXPR _rows "${_rows} + 1")
      if(CMAKE_MATCH_1 STREQUAL "0")
        math(EXPR _unsupported "${_unsupported} + 1")
      endif()
    endif()
  elseif(_line MATCHES "^INSERT INTO test_backend_ops ")
    math(EXPR _rows "${_rows} + 1")
    # Preserve SQL verbatim, checking only the pinned result schema needed to
    # reject skipped/unsupported/failed runs and non-positive measurements.
    if(_line MATCHES "^INSERT INTO test_backend_ops [(][^)]*[)] VALUES [(]'[^']*', '[^']*', '[^']*', '[^']*', '[^']*', 'perf', '([01])', '([01])', '[^']*', '([0-9]+[.][0-9]+)', '[^']*', '[^']*', '[0-9]+', '([0-9]+)', '[^']*', '[^']*'[)];$")
      set(_supported "${CMAKE_MATCH_1}")
      set(_passed "${CMAKE_MATCH_2}")
      set(_time "${CMAKE_MATCH_3}")
      set(_runs "${CMAKE_MATCH_4}")
      if(_supported STREQUAL "0")
        math(EXPR _unsupported "${_unsupported} + 1")
      elseif(_passed STREQUAL "1" AND _time GREATER 0 AND _runs GREATER 0)
        math(EXPR _measured "${_measured} + 1")
      else()
        math(EXPR _invalid "${_invalid} + 1")
      endif()
    else()
      math(EXPR _invalid "${_invalid} + 1")
    endif()
  endif()
endforeach()

if(_rows EQUAL 0)
  file(APPEND "${_result}" "validation=failed_no_results\n")
  message(FATAL_ERROR "ggml produced no matching results. Check the exact device name, operation and parameter regex. Output: ${_output}; errors: ${_stderr}")
endif()
if(PROBE_MODE STREQUAL "perf" AND (_measured EQUAL 0 OR _invalid GREATER 0))
  file(APPEND "${_result}" "validation=failed_no_valid_measurement\nrows=${_rows}\nmeasured=${_measured}\nunsupported=${_unsupported}\ninvalid=${_invalid}\n")
  message(FATAL_ERROR "ggml perf did not produce valid measured kernels (measured=${_measured}, unsupported=${_unsupported}, invalid=${_invalid}). Output: ${_output}; errors: ${_stderr}")
endif()
file(APPEND "${_result}" "validation=passed\nrows=${_rows}\nmeasured=${_measured}\nunsupported=${_unsupported}\n")
message(STATUS "ggml ${PROBE_MODE}: ${_rows} result(s), ${_measured} measured, ${_unsupported} unsupported. Output: ${_output}")
