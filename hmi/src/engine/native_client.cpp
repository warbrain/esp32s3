#include "engine/native_client.hpp"
#include "engine/sdk_library.hpp"
#include "base/logger.hpp"

#include <audiocpp.h>

#include <windows.h>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hmi::engine
{
namespace
{
// ABI of the vendored header; a mismatch means the DLL and this client were
// built from different engine revisions and must not be mixed.
constexpr std::uint32_t kExpectedAbi = (AUDIOCPP_ABI_VERSION_MAJOR << 16) |
    (AUDIOCPP_ABI_VERSION_MINOR << 8) | AUDIOCPP_ABI_VERSION_PATCH;
constexpr const char *kDefaultFamilies = "qwen3_asr,qwen3_tts";

class InitializationTimer
{
public:
    InitializationTimer(const SdkInitializationObserver &observer, const char *name)
        : observer_(observer), name_(name)
    {
        publish(false, false, 0.0, {});
        started_ = Clock::now();
    }

    ~InitializationTimer()
    {
        if (!finished_) finish(false, "initialization stage did not complete");
    }

    void finish(bool success, std::string detail = {}) noexcept
    {
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started_).count();
        finished_ = true;
        publish(true, success, elapsed, std::move(detail));
    }

private:
    using Clock = std::chrono::steady_clock;
    void publish(bool completed, bool success, double elapsed, std::string detail) noexcept
    {
        const DWORD saved_error = GetLastError();
        try
        {
            if (observer_) observer_({name_, completed, success, elapsed, std::move(detail)});
        }
        catch (...) {}
        SetLastError(saved_error);
    }

    const SdkInitializationObserver &observer_;
    const char *name_;
    Clock::time_point started_{};
    bool finished_ = false;
};

std::wstring environment(const wchar_t *name)
{
    wchar_t *value = nullptr;
    std::size_t size = 0;
    if (_wdupenv_s(&value, &size, name) != 0 || value == nullptr)
        return {};
    std::wstring text(value);
    std::free(value);
    return text;
}

std::string to_utf8(const std::wstring &text)
{
    if (text.empty())
        return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string utf8(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        utf8.data(), required, nullptr, nullptr);
    return utf8;
}

std::wstring engine_config_path(std::string *error)
{
    const auto directory = base::process_directory();
    auto configured = environment(L"HMI_NATIVE_CONFIG");
    if (configured.empty()) configured = environment(L"AUDIOCPP_CONFIG");
    const auto selected = configured.empty() ? directory / L"config/engine.json" :
        std::filesystem::path(configured);
    std::filesystem::path resolved;
    if (!base::resolve_project_path(selected, directory, resolved, error)) return {};
    return resolved.native();
}

// The engine takes a config *file* (flat key/value), not a directory.
bool ensure_engine_config(const std::wstring &path, std::string *error)
{
    std::error_code code;
    if (std::filesystem::exists(std::filesystem::path(path), code))
        return true;

    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, code);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        if (error)
            *error = "cannot write engine config: " + to_utf8(path);
        return false;
    }
    file << "{\n  \"families\": \"" << kDefaultFamilies << "\"\n}\n";
    return true;
}
}

struct Client::Impl
{
    detail::SdkRuntimeDependencies runtime_dependencies;
    HMODULE module = nullptr;
    audiocpp_registry *registry = nullptr;
    Version version;
    bool ready = false;
    base::Logger *logger = nullptr;
    audiocpp_diagnostics *diagnostics = nullptr;
    decltype(&::audiocpp_diagnostics_create) diagnostics_create = nullptr;
    decltype(&::audiocpp_diagnostics_free) diagnostics_free = nullptr;
    decltype(&::audiocpp_diagnostics_capabilities) diagnostics_capabilities = nullptr;
    decltype(&::audiocpp_diagnostics_statistics) diagnostics_statistics = nullptr;
    decltype(&::audiocpp_diagnostics_get_snapshot) diagnostics_get_snapshot = nullptr;

    decltype(&::audiocpp_abi_version) abi_version = nullptr;
    decltype(&::audiocpp_build_version) build_version = nullptr;
    decltype(&::audiocpp_last_error) last_error = nullptr;
    decltype(&::audiocpp_status_string) status_string = nullptr;
    decltype(&::audiocpp_registry_create) registry_create = nullptr;
    decltype(&::audiocpp_registry_free) registry_free = nullptr;
    decltype(&::audiocpp_registry_family_count) family_count = nullptr;
    decltype(&::audiocpp_registry_family) family = nullptr;
    decltype(&::audiocpp_task_count) task_count = nullptr;
    decltype(&::audiocpp_task_name) task_name = nullptr;

    bool bind_symbols(std::string *error);
};

bool Client::Impl::bind_symbols(std::string *error)
{
    const auto load = [this](const char *name, auto &target) {
        target = reinterpret_cast<std::decay_t<decltype(target)>>(GetProcAddress(module, name));
        return target != nullptr;
    };

    const bool bound = load("audiocpp_abi_version", abi_version) &&
                       load("audiocpp_build_version", build_version) &&
                       load("audiocpp_last_error", last_error) &&
                       load("audiocpp_status_string", status_string) &&
                       load("audiocpp_registry_create", registry_create) &&
                       load("audiocpp_registry_free", registry_free) &&
                       load("audiocpp_registry_family_count", family_count) &&
                       load("audiocpp_registry_family", family) &&
                       load("audiocpp_task_count", task_count) &&
                       load("audiocpp_task_name", task_name);
    if (!bound && error)
        *error = "native.dll is missing a required engine interface";
    load("audiocpp_diagnostics_create", diagnostics_create);
    load("audiocpp_diagnostics_free", diagnostics_free);
    load("audiocpp_diagnostics_capabilities", diagnostics_capabilities);
    load("audiocpp_diagnostics_statistics", diagnostics_statistics);
    load("audiocpp_diagnostics_get_snapshot", diagnostics_get_snapshot);
    return bound;
}

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client()
{
    close();
}

bool Client::open(std::string *error)
{
    return open(error, {});
}

bool Client::open(std::string *error, const SdkInitializationObserver &observer)
{
    return open(error, observer, nullptr);
}

bool Client::open(std::string *error, const SdkInitializationObserver &observer, base::Logger *logger)
{
    const auto stage = [&](const char *name, const auto &operation) {
        InitializationTimer timer(observer, name);
        const bool ok = operation();
        timer.finish(ok, !ok && error ? *error : std::string{});
        return ok;
    };
    stage("close_previous", [&] { close(); return true; });
    Impl &impl = *impl_;
    impl.logger = logger;

    std::filesystem::path library;
    if (!stage("resolve_path", [&] {
        library = detail::sdk_library_path(error);
        if (library.empty()) return false;
        std::error_code code;
        if (std::filesystem::exists(library, code)) return true;
        if (error) *error = "native engine not found: " + to_utf8(library.wstring()) +
            " (rebuild this project's hmi_runtime target to deploy the SDK)";
        return false;
    })) return false;

    if (!stage("dependency_preflight", [&] { return detail::sdk_preflight(library, error); })) return false;
    if (!stage("dependency_preload", [&] {
        return detail::load_preflighted_sdk_runtime_dependencies(library, impl.runtime_dependencies, error);
    })) return false;
    // The dependency owner has already validated this complete SDK set.
    if (!stage("native_library_load", [&] {
        impl.module = detail::load_preflighted_sdk_library(library, error);
        return impl.module != nullptr;
    }))
    {
        close();
        return false;
    }

    if (!stage("bind_symbols", [&] { return impl.bind_symbols(error); }))
    {
        close();
        return false;
    }

    if (!stage("abi_check", [&] {
        impl.version.abi = impl.abi_version();
        const char *build = impl.build_version();
        impl.version.build = build != nullptr ? build : "";
        if (impl.version.abi == kExpectedAbi) return true;
        if (error) *error = "native ABI mismatch: expected " + std::to_string(kExpectedAbi) +
            ", got " + std::to_string(impl.version.abi);
        return false;
    }))
    {
        close();
        return false;
    }

    if (logger)
    {
        InitializationTimer diagnostic_timer(observer, "diagnostics");
        std::string detail = "event=native_diagnostics timing=unavailable nvtx=unavailable";
        if (impl.diagnostics_create && impl.diagnostics_free && impl.diagnostics_capabilities)
        {
            std::filesystem::path path;
            std::string diagnostic_error;
            const auto candidate = logger->options().file.parent_path() /
                ("native-" + std::to_string(GetCurrentProcessId()) + ".log");
            if (base::resolve_project_path(candidate, base::process_directory(), path, &diagnostic_error) &&
                impl.diagnostics_create(to_utf8(path.wstring()).c_str(), &impl.diagnostics) == AUDIOCPP_OK &&
                impl.diagnostics)
            {
                const auto capabilities = impl.diagnostics_capabilities();
                detail = "event=native_diagnostics timing=aggregate file=" + to_utf8(path.wstring()) +
                    " nvtx=" + ((capabilities & 2u) != 0 ? "compiled" : "unavailable") +
                    " gpu_counters=external_profiler";
            }
            else
            {
                if (diagnostic_error.empty()) diagnostic_error = impl.last_error();
                detail += " detail=" + diagnostic_error;
            }
        }
        logger->write(impl.diagnostics ? base::LogLevel::Info : base::LogLevel::Warning, "engine", detail);
        diagnostic_timer.finish(true);
    }

    std::wstring config;
    if (!stage("configuration", [&] {
        config = engine_config_path(error);
        return !config.empty() && ensure_engine_config(config, error);
    }))
    {
        close();
        return false;
    }

    if (!stage("registry_and_backends", [&] {
        const std::string config_utf8 = to_utf8(config);
        const audiocpp_status status = impl.registry_create(config_utf8.c_str(), &impl.registry);
        if (status == AUDIOCPP_OK && impl.registry != nullptr) return true;
        if (error)
        {
            const char *detail = impl.last_error();
            *error = "native registry creation failed";
            if (detail != nullptr && *detail != '\0')
                *error += std::string(": ") + hmi::engine::detail::native_diagnostic(detail);
            *error += " (config: " + config_utf8 + ")";
        }
        return false;
    }))
    {
        close();
        return false;
    }

    impl.ready = true;
    return true;
}

void Client::close()
{
    Impl &impl = *impl_;
    if (impl.registry != nullptr && impl.registry_free != nullptr)
        impl.registry_free(impl.registry);
    impl.registry = nullptr;
    if (impl.diagnostics && (impl.diagnostics_statistics || impl.diagnostics_get_snapshot) && impl.logger)
    {
        std::uint64_t lines = 0, failures = 0;
        audiocpp_diagnostic_snapshot snapshot{};
        snapshot.struct_size = static_cast<std::uint32_t>(sizeof(snapshot));
        const bool snapshot_ready = impl.diagnostics_get_snapshot &&
            impl.diagnostics_get_snapshot(impl.diagnostics, &snapshot) == AUDIOCPP_OK &&
            snapshot.version == AUDIOCPP_DIAGNOSTIC_SNAPSHOT_VERSION;
        if (snapshot_ready) { lines = snapshot.lines; failures = snapshot.write_failures; }
        else if (impl.diagnostics_statistics) impl.diagnostics_statistics(impl.diagnostics, &lines, &failures);
        const std::string operation_fields = snapshot_ready ?
            " snapshot_version=" + std::to_string(snapshot.version) +
            " completed_operations=" + std::to_string(snapshot.completed_operations) +
            " successful_operations=" + std::to_string(snapshot.successful_operations) +
            " failed_operations=" + std::to_string(snapshot.failed_operations) +
            " suppressed_operations=" + std::to_string(snapshot.suppressed_operations) +
            " top_level_operations=" + std::to_string(snapshot.top_level_operations) +
            " operation_wall_ms=" + std::to_string(snapshot.operation_wall_ms) +
            " top_level_wall_ms=" + std::to_string(snapshot.top_level_wall_ms) +
            " model_load_wall_ms=" + std::to_string(snapshot.model_load_wall_ms) +
            " session_create_wall_ms=" + std::to_string(snapshot.session_create_wall_ms) +
            " session_run_wall_ms=" + std::to_string(snapshot.session_run_wall_ms) +
            " stream_wall_ms=" + std::to_string(snapshot.stream_wall_ms) +
            " operation_timing=inclusive_scope_sum top_level_timing=concurrent_scope_sum" :
            " operation_snapshot=unavailable";
        impl.logger->write(failures ? base::LogLevel::Warning : base::LogLevel::Info, "engine",
            "event=native_diagnostics_summary scope=shared_file_sink_snapshot excludes_closing_summary=1 lines=" + std::to_string(lines) +
            " write_failures=" + std::to_string(failures) + operation_fields);
    }
    if (impl.diagnostics && impl.diagnostics_free) impl.diagnostics_free(impl.diagnostics);
    impl.diagnostics = nullptr;

    if (impl.module != nullptr)
    {
        FreeLibrary(impl.module);
        impl.module = nullptr;
    }
    impl.runtime_dependencies.clear();
    // A failed open can leave a partially bound table, and a successful open
    // leaves these addresses in the unloaded DLL. Never retain callable
    // exports after releasing the module; the copied version is safe to keep.
    impl.abi_version = nullptr;
    impl.build_version = nullptr;
    impl.last_error = nullptr;
    impl.status_string = nullptr;
    impl.registry_create = nullptr;
    impl.registry_free = nullptr;
    impl.family_count = nullptr;
    impl.family = nullptr;
    impl.task_count = nullptr;
    impl.task_name = nullptr;
    impl.diagnostics_create = nullptr;
    impl.diagnostics_free = nullptr;
    impl.diagnostics_capabilities = nullptr;
    impl.diagnostics_statistics = nullptr;
    impl.diagnostics_get_snapshot = nullptr;
    impl.logger = nullptr;
    impl.ready = false;
}

bool Client::ready() const noexcept
{
    return impl_->ready;
}

void *Client::module_handle() const noexcept
{
    return impl_->module;
}

Version Client::version() const noexcept
{
    return impl_->version;
}

std::vector<std::string> Client::families() const
{
    std::vector<std::string> result;
    const Impl &impl = *impl_;
    if (impl.registry == nullptr || impl.family_count == nullptr || impl.family == nullptr)
        return result;

    const std::size_t count = impl.family_count(impl.registry);
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const char *name = nullptr;
        if (impl.family(impl.registry, index, &name) == AUDIOCPP_OK && name != nullptr)
            result.emplace_back(name);
    }
    return result;
}

std::vector<std::string> Client::tasks() const
{
    std::vector<std::string> result;
    const Impl &impl = *impl_;
    if (impl.task_count == nullptr || impl.task_name == nullptr)
        return result;

    const std::size_t count = impl.task_count();
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const char *name = impl.task_name(index);
        if (name != nullptr)
            result.emplace_back(name);
    }
    return result;
}

audiocpp_registry *Client::registry() const noexcept
{
    return impl_->registry;
}

std::string Client::last_error() const
{
    const Impl &impl = *impl_;
    if (impl.last_error == nullptr)
        return {};
    const char *detail = impl.last_error();
    return detail != nullptr ? hmi::engine::detail::native_diagnostic(detail) : std::string{};
}
}
