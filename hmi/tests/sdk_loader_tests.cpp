#include "engine/sdk_library.hpp"

#include <cstdio>

namespace
{
struct ScopedPath
{
    std::wstring previous;
    bool existed = false;

    ScopedPath()
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD required = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        existed = required != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        if (required)
        {
            previous.resize(required);
            const DWORD count = GetEnvironmentVariableW(L"PATH", previous.data(), required);
            previous.resize(count);
        }
    }

    bool prepend(const std::filesystem::path &directory) const
    {
        const auto path = directory.wstring() + L";" + previous;
        return SetEnvironmentVariableW(L"PATH", path.c_str()) != FALSE;
    }

    ~ScopedPath() { SetEnvironmentVariableW(L"PATH", existed ? previous.c_str() : nullptr); }
};
}

int main()
{
    int failures = 0;
    const auto check = [&](bool value, const char *description) {
        std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
        if (!value) ++failures;
    };
    wchar_t executable[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    const auto base = std::filesystem::path(std::wstring(executable, count)).parent_path();
    check(std::filesystem::is_regular_file(base / "hmi_test_sdk_dependency.dll"),
        "old same-name dependency exists beside executable");
    const auto sdk = base / "current" / "hmi_test_sdk_fixture.dll";
    std::error_code code;
    const auto relative = std::filesystem::relative(sdk, std::filesystem::current_path(), code);
    check(!code && relative.is_relative(), "explicit SDK path starts relative");
    wchar_t previous[32768]{};
    const DWORD previous_count = GetDllDirectoryW(32768, previous);
    std::string error;
    const HMODULE loaded = hmi::engine::detail::load_sdk_library(relative, &error);
    check(loaded != nullptr, "SDK fixture loads without legacy import collision");
    if (!loaded) std::printf("%s\n", error.c_str());
    using Version = int (*)();
    const auto version = loaded ? reinterpret_cast<Version>(
        GetProcAddress(loaded, "hmi_test_sdk_fixture_version")) : nullptr;
    check(version != nullptr && version() == 22,
        "SDK loads its matching dependency instead of executable's old dependency");
    wchar_t after[32768]{};
    const DWORD after_count = GetDllDirectoryW(32768, after);
    check(previous_count == after_count && std::wstring(previous, previous_count) ==
        std::wstring(after, after_count), "process DLL search directory is unchanged");
    if (loaded) FreeLibrary(loaded);
    const HMODULE missing = hmi::engine::detail::load_sdk_library(
        base / "current" / "missing.dll", &error);
    check(missing == nullptr && !error.empty(), "missing explicit SDK reports a load failure");
    if (missing) FreeLibrary(missing);
    const auto runtime = base / "runtime-only" / "hmi_test_external_runtime.dll";
    check(std::filesystem::is_regular_file(runtime) &&
        !std::filesystem::exists(base / runtime.filename()) &&
        !std::filesystem::exists(base / "current" / runtime.filename()),
        "external runtime exists only outside executable and SDK directories");
    const ScopedPath original_path;
    {
        const ScopedPath temporary_path;
        check(temporary_path.prepend(runtime.parent_path()), "runtime directory added to temporary PATH");
        const HMODULE path_fixture = hmi::engine::detail::load_sdk_library(
            base / "current" / "hmi_test_sdk_path_fixture.dll", &error);
        if (!path_fixture) std::printf("%s\n", error.c_str());
        const auto runtime_version = path_fixture ? reinterpret_cast<Version>(
            GetProcAddress(path_fixture, "hmi_test_path_fixture_version")) : nullptr;
        check(runtime_version != nullptr && runtime_version() == 33,
            "SDK can load installed runtime dependencies from PATH");
        if (path_fixture) FreeLibrary(path_fixture);
    }
    const ScopedPath restored_path;
    check(original_path.existed == restored_path.existed && original_path.previous == restored_path.previous,
        "temporary PATH change is restored");
    std::printf("%d failure(s); no model inference, input, or playback\n", failures);
    return failures == 0 ? 0 : 1;
}
