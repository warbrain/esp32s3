# Bundled native SDK

`source/UPSTREAM_REVISION` pins the upstream ShugoAI SDK snapshot. The SDK stages
that commit in `sdk-manifest.json` alongside the ABI and file hashes. Build and
deploy `native.dll` and the GGML DLLs from the same configuration.

The imported tree includes the engine, model specifications, vendored dependencies,
and inputs required by the upstream CMake configuration. HMI selects Qwen3-ASR and
Qwen3-TTS. The upstream `audiocpp.h` and C facade retain their original ABI.

HMI code in `hmi-extension` adds the PCM frontend and optional dual-channel
H-GTCRN C entry points. **HMI LOGGING** marks log forwarding and the local file
sink. **HMI TELEMETRY** marks operation timing, snapshots, and counters. The
application obtains these additions through `audiocpp_hmi.h`. The upstream
diagnostics sources remain in `source` as the reference for synchronization.

The source patches marked **HMI** preserve existing behavior: Silero frame
probability, GTCRN thread configuration and H-GTCRN support, Qwen3-TTS stop
reasons used to recover truncated speech, and Windows build options for GGML.

## macOS Metal SDK

Use the native SDK entry point from the repository root on an Apple Silicon Mac
with Xcode and its selected command line tools. The SDK uses the pinned upstream
Metal backend, links GGML statically, and embeds the shaders in the SDK library.
It uses the same HMI C extensions as the Windows SDK. The default profile uses
Qwen3-ASR and Qwen3-TTS and can build without an external OpenMP runtime.

```sh
cmake -S hmi/3rd_party/native -B hmi/build/macos-metal \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 -DENGINE_ENABLE_METAL=ON
cmake --build hmi/build/macos-metal --config Release --parallel
ctest --test-dir hmi/build/macos-metal -C Release --output-on-failure
cmake --install hmi/build/macos-metal --config Release --prefix hmi/build/macos-metal/sdk
```

The Metal smoke test opens the backend explicitly and checks a matrix product
on that device. With Python 3 available, CTest also verifies the Mach-O C exports
against the two SDK headers. The install contains `libaudiocpp.dylib`, both
headers, third-party notices, and the upstream revision. Select Metal for an
inference session with `audiocpp_backend_config.backend = "metal"`. A macOS CPU
build can use `-DENGINE_ENABLE_METAL=OFF` in a separate build directory.
