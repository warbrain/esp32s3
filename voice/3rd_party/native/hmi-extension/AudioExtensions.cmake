include_guard(GLOBAL)

function(hmi_enable_audio_extensions source_root extension_dir)
  # HMI EXTENSION. The wrapper adds exports around the pinned C facade.
  # Upstream's Mach-O and ELF export lists already allow audiocpp_*.
  get_target_property(_facade_sources audiocpp SOURCES)
  list(FILTER _facade_sources EXCLUDE REGEX "(^|/)src/capi/audiocpp[.]cpp$")
  set_property(TARGET audiocpp PROPERTY SOURCES ${_facade_sources}
    "${extension_dir}/hmi_audiocpp.cpp" "${extension_dir}/audio_frontend.cpp")
  target_include_directories(audiocpp PRIVATE "${source_root}/src/capi" "${extension_dir}")

  # HMI LOG / TELEMETRY. Keep this sink outside the upstream source tree.
  get_target_property(_engine_sources engine_core SOURCES)
  list(FILTER _engine_sources EXCLUDE REGEX "(^|/)src/framework/debug/trace[.]cpp$")
  set_property(TARGET engine_core PROPERTY SOURCES ${_engine_sources}
    "${extension_dir}/native_telemetry.cpp")
  target_include_directories(engine_core PRIVATE "${extension_dir}")

  if(TARGET ggml-cuda)
    foreach(_include_dir IN LISTS CUDAToolkit_INCLUDE_DIRS)
      if(EXISTS "${_include_dir}/nvtx3/nvToolsExt.h")
        set_property(SOURCE "${extension_dir}/native_telemetry.cpp"
          TARGET_DIRECTORY engine_core APPEND PROPERTY INCLUDE_DIRECTORIES "${_include_dir}")
        set_property(SOURCE "${extension_dir}/native_telemetry.cpp"
          TARGET_DIRECTORY engine_core APPEND PROPERTY COMPILE_DEFINITIONS ENGINE_HAS_NVTX=1)
        break()
      endif()
    endforeach()
  endif()
endfunction()
