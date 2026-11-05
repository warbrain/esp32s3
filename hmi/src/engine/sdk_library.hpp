#pragma once

#include <windows.h>

#include "engine/sdk_preflight.hpp"
#include "base/project_paths.hpp"
#include "engine/native_diagnostics.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace hmi::engine::detail
{
template<class Exists>
std::filesystem::path select_sdk_library_path(const std::filesystem::path &base,
                                              const std::filesystem::path &configured,
                                              Exists exists)
{
    if (!configured.empty()) return configured;
    const std::filesystem::path candidates[] = {
        base / "native" / "native.dll",
        base / "native.dll"
    };
    for (const auto &candidate : candidates)
        if (exists(candidate)) return candidate;
    return candidates[0];
}

inline std::filesystem::path absolute_sdk_library_path(const std::filesystem::path &library,
                                                       std::string *error)
{
    // Preserve the loader's explicit relative-path contract before applying
    // the project boundary; callers may supply a path relative to the CWD.
    std::error_code code;
    const auto selected = std::filesystem::absolute(library, code);
    if (code)
    {
        if (error) *error = "cannot resolve native SDK path: " + code.message();
        return {};
    }
    std::filesystem::path absolute;
    std::string detail;
    if (!base::resolve_project_path(selected, base::process_directory(), absolute, &detail))
    {
        if (error) *error = "native SDK path rejected: " + detail;
        return {};
    }
    return absolute;
}

inline std::filesystem::path sdk_library_path(std::string *error = nullptr)
{
    wchar_t *configured = nullptr;
    std::size_t size = 0;
    _wdupenv_s(&configured, &size, L"HMI_NATIVE_DLL");
    std::filesystem::path override = configured ? configured : L"";
    std::free(configured);
    if (override.empty())
    {
        configured = nullptr;
        size = 0;
        _wdupenv_s(&configured, &size, L"AUDIOCPP_DLL");
        override = configured ? configured : L"";
        std::free(configured);
    }
    wchar_t module[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, module, 32768);
    const auto base = std::filesystem::path(std::wstring(module, count)).parent_path();
    const auto selected = select_sdk_library_path(base, override, [](const auto &candidate) {
        std::error_code code;
        const bool exists = std::filesystem::exists(candidate, code);
        // An unreadable candidate must fail at load, never silently select old code.
        return exists || static_cast<bool>(code);
    });
    return absolute_sdk_library_path(selected, error);
}

// Only call after sdk_preflight has validated the complete SDK directory.
inline HMODULE load_preflighted_sdk_library(const std::filesystem::path &absolute, std::string *error)
{
    // Error modes are thread local; never change and restore a process-global
    // policy while other SDK/model threads may be loading dependencies.
    struct ErrorModeGuard
    {
        DWORD previous = 0;
        bool active = false;
        ErrorModeGuard() : active(SetThreadErrorMode(GetThreadErrorMode() |
            SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous) != FALSE) {}
        ~ErrorModeGuard()
        {
            const DWORD last = GetLastError();
            if (active) SetThreadErrorMode(previous, nullptr);
            SetLastError(last);
        }
    } guard;
    if (!guard.active)
    {
        if (error) *error = "cannot set thread-local DLL error policy (win32 " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    // Prefer this SDK's dependencies while retaining installed GPU runtimes on PATH.
    const HMODULE module = LoadLibraryExW(absolute.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
    {
        const DWORD last = GetLastError();
        if (error)
        {
            const auto path = absolute.generic_u8string();
            *error = "LoadLibraryExW failed for " + std::string(path.begin(), path.end()) +
                " (win32 " + std::to_string(last) + ")";
        }
    }
    else if (error) error->clear();
    return module;
}

inline HMODULE load_sdk_library(const std::filesystem::path &library, std::string *error)
{
    const auto absolute = absolute_sdk_library_path(library, error);
    if (absolute.empty() || !sdk_preflight(absolute, error)) return nullptr;
    return load_preflighted_sdk_library(absolute, error);
}

class SdkRuntimeDependencies
{
public:
    SdkRuntimeDependencies() = default;
    ~SdkRuntimeDependencies() { clear(); }
    SdkRuntimeDependencies(const SdkRuntimeDependencies &) = delete;
    SdkRuntimeDependencies &operator=(const SdkRuntimeDependencies &) = delete;

    // Release only after every SDK registry, model, backend and DLL using these
    // dependencies has gone away. No module is pinned or process-global.
    void clear() noexcept
    {
        for (auto item = modules_.rbegin(); item != modules_.rend(); ++item) FreeLibrary(*item);
        modules_.clear();
    }
    bool empty() const noexcept { return modules_.empty(); }

private:
    friend bool load_sdk_runtime_dependencies(const std::filesystem::path &, SdkRuntimeDependencies &, std::string *);
    friend bool load_preflighted_sdk_runtime_dependencies(const std::filesystem::path &, SdkRuntimeDependencies &, std::string *);
    std::vector<HMODULE> modules_;
};

// Only call after sdk_preflight has validated this complete SDK set.
inline bool load_preflighted_sdk_runtime_dependencies(const std::filesystem::path &library,
                                                        SdkRuntimeDependencies &owner, std::string *error)
{
    if (!owner.empty())
    {
        if (error) *error = "SDK runtime dependencies are already owned; close the SDK before replacing them";
        return false;
    }
    const auto absolute = absolute_sdk_library_path(library, error);
    if (absolute.empty()) return false;
    const auto directory = absolute.parent_path();
    try
    {
        std::error_code code;
        bool present = false;
        for (std::filesystem::directory_iterator it(directory, code), end; it != end; it.increment(code))
        {
            if (code) throw SdkPreflightError(ERROR_PATH_NOT_FOUND, "cannot enumerate SDK runtime dependencies");
            const auto name = sdk_fold_path(it->path().filename());
            if (name.starts_with(L"cudnn") && name.ends_with(L".dll")) present = true;
        }
        if (code) throw SdkPreflightError(ERROR_PATH_NOT_FOUND, "cannot enumerate SDK runtime dependencies");
        if (!present)
        {
            if (error) error->clear();
            return true; // CPU/Vulkan or CUDA SDK built without cuDNN.
        }
        // cuDNN 9.27 loads its split components lazily by basename, after the
        // initial LOAD_WITH_ALTERED_SEARCH_PATH scope ends. Retain the entire
        // matching local set before any SDK call can trigger that lookup.
        const wchar_t *components[] = {
            L"cudnn64_9.dll", L"cudnn_graph64_9.dll", L"cudnn_ops64_9.dll",
            L"cudnn_cnn64_9.dll", L"cudnn_adv64_9.dll", L"cudnn_heuristic64_9.dll",
            L"cudnn_engines_tensor_ir64_9.dll", L"cudnn_engines_precompiled64_9.dll",
            L"cudnn_engines_runtime_compiled64_9.dll", L"cudnn_ext64_9.dll"
        };
        for (const auto *name : components)
        {
            const auto path = directory / name;
            if (!std::filesystem::is_regular_file(path, code) || code)
                throw SdkPreflightError(ERROR_MOD_NOT_FOUND, "packaged cuDNN component is missing: " + sdk_path_utf8(path));
            const HMODULE loaded = GetModuleHandleW(name);
            if (loaded)
            {
                wchar_t filename[32768]{};
                const DWORD count = GetModuleFileNameW(loaded, filename, 32768);
                if (!count || sdk_fold_path(std::filesystem::path(std::wstring(filename, count))) != sdk_fold_path(path))
                    throw SdkPreflightError(ERROR_PROC_NOT_FOUND, "cuDNN component already loaded from a different directory: " + sdk_path_utf8(path.filename()));
            }
        }
        SdkRuntimeDependencies pending;
        pending.modules_.reserve(sizeof(components) / sizeof(components[0]));
        for (const auto *name : components)
        {
            const auto path = directory / name;
            const HMODULE loaded = load_preflighted_sdk_library(path, error);
            if (!loaded) return false;
            pending.modules_.push_back(loaded);
        }
        owner.modules_.swap(pending.modules_);
        if (error) error->clear();
        return true;
    }
    catch (const SdkPreflightError &failure)
    {
        if (error) *error = "SDK runtime dependencies failed (win32 " + std::to_string(failure.code) + "): " + failure.what();
        SetLastError(failure.code);
    }
    catch (const std::exception &failure)
    {
        if (error) *error = std::string("SDK runtime dependencies failed: ") + failure.what();
        SetLastError(ERROR_DLL_INIT_FAILED);
    }
    return false;
}

inline bool load_sdk_runtime_dependencies(const std::filesystem::path &library,
                                           SdkRuntimeDependencies &owner, std::string *error)
{
    const auto absolute = absolute_sdk_library_path(library, error);
    if (absolute.empty() || !sdk_preflight(absolute, error)) return false;
    return load_preflighted_sdk_runtime_dependencies(absolute, owner, error);
}

inline HMODULE retain_sdk_library(void *module)
{
    if (!module) return nullptr;
    HMODULE retained = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(module), &retained) ? retained : nullptr;
}
}
