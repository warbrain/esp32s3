#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct audiocpp_registry;
namespace hmi::base { class Logger; }

namespace hmi::engine
{
struct Version
{
    std::uint32_t abi = 0;
    std::string build;
};

struct SdkInitializationStage
{
    std::string name;
    bool completed = false;
    bool success = false;
    double elapsed_ms = 0.0;
    std::string detail;
};

using SdkInitializationObserver = std::function<void(const SdkInitializationStage &)>;

// Owns the native engine shared library and its model registry.
//
// The DLL is loaded dynamically from HMI_NATIVE_DLL or next to the executable;
// a missing engine or a missing model is a normal state, not a crash.
class Client
{
public:
    Client();
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    bool open(std::string *error);
    // Called on the opening thread, before and after each initialization stage.
    // Observer failures never change SDK initialization or its Win32 error.
    bool open(std::string *error, const SdkInitializationObserver &observer);
    // Optional diagnostics are owned by this client. Missing diagnostic exports
    // do not prevent initialization. The borrowed logger must outlive close().
    bool open(std::string *error, const SdkInitializationObserver &observer, base::Logger *logger);
    void close();
    bool ready() const noexcept;

    Version version() const noexcept;
    std::vector<std::string> families() const; // ex: {"qwen3_asr", "qwen3_tts", ...}
    std::vector<std::string> tasks() const;    // canonical task tokens

    // Borrowed handle; valid until close(). Session wrappers pass it to
    // the native model loader.
    audiocpp_registry *registry() const noexcept;

    // Last error reported by the engine on this thread.
    std::string last_error() const;

private:
    friend class AsrSession;
    friend class TtsSession;
    friend class AudioPreprocessor;
    friend class HgtcrnSession;
    void *module_handle() const noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
