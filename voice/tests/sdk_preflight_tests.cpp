#include "engine/sdk_library.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>

namespace
{
void report(const std::string &text)
{
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

int child(const std::wstring &mode, const std::filesystem::path &target,
          const std::filesystem::path &marker, const std::filesystem::path &old)
{
    SetEnvironmentVariableW(L"HMI_TEST_SDK_MARKER", marker.c_str());
    if (mode == L"optional-owner" || mode == L"partial-owner")
    {
        hmi::engine::detail::SdkRuntimeDependencies owner;
        std::string error;
        const bool loaded = hmi::engine::detail::load_sdk_runtime_dependencies(target, owner, &error);
        const bool ok = mode == L"optional-owner" ? loaded && owner.empty() :
            !loaded && owner.empty() && error.find("win32 126") != std::string::npos &&
            GetModuleHandleW(L"cudnn64_9.dll") == nullptr;
        if (!ok) report("runtime-owner failure: " + error + "\n");
        return ok ? 0 : 1;
    }
    HMODULE legacy = nullptr;
    if (mode == L"loaded-conflict")
    {
        legacy = LoadLibraryExW(old.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!legacy) return 10;
    }
    const DWORD before = GetThreadErrorMode();
    std::string error;
    const HMODULE loaded = hmi::engine::detail::load_sdk_library(target, &error);
    const DWORD after = GetThreadErrorMode();
    bool ok = false;
    if (mode == L"good")
    {
        using Version = int (*)();
        const auto version = loaded ? reinterpret_cast<Version>(GetProcAddress(loaded, "hmi_test_sdk_preflight_version")) : nullptr;
        ok = loaded && version && version() == 22 && std::filesystem::exists(marker) && before == after;
    }
    else if (mode == L"bad")
        ok = !loaded && error.find("SDK preflight failed (win32 127)") != std::string::npos &&
            error.find("ggml-test-dependency.dll!ggml_fixture_") != std::string::npos && !std::filesystem::exists(marker);
    else if (mode == L"loaded-conflict")
        ok = !loaded && error.find("already loaded from a different directory") != std::string::npos && !std::filesystem::exists(marker);
    else if (mode == L"missing-cudnn")
        ok = !loaded && error.find("SDK preflight failed (win32 126)") != std::string::npos &&
            error.find("cudnn-") != std::string::npos;
    else if (mode == L"invalid")
        ok = !loaded && error.find("win32 193") != std::string::npos;
    else if (mode == L"missing")
        ok = !loaded && error.find("cannot read SDK file") != std::string::npos;
    if (loaded) FreeLibrary(loaded);
    if (legacy) FreeLibrary(legacy);
    if (!ok) report("child failure: " + error + "\n");
    return ok ? 0 : 1;
}

std::wstring argument(const std::wstring &text) { return L"\"" + text + L"\""; }
}

int wmain(int argc, wchar_t **argv)
{
    // Isolated fixture process policy: even a regression must not display a
    // Windows loader dialog or hang CTest. Production loader never sets this.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (argc == 6 && std::wstring_view(argv[1]) == L"--child")
        return child(argv[2], argv[3], argv[4], argv[5]);
    wchar_t executable[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    const std::filesystem::path own(std::wstring(executable, count));
    const auto base = own.parent_path();
    const auto fixture = L"hmi_test_sdk_preflight.dll";
    const auto old = base / L"bad/ggml-test-dependency.dll";
    const auto invalid = base / L"invalid.dll";
    { std::ofstream file(invalid, std::ios::binary); file << "not a PE image"; }
    // Rename only the import basename in a local copy, preserving the PE
    // layout. A missing packaged cuDNN must be rejected before native loading.
    const auto missing_cudnn = base / L"missing-cudnn.dll";
    {
        std::ifstream input(base / L"good" / fixture, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const std::string original = "ggml-test-dependency.dll";
        const std::string renamed = "cudnn-" + std::string(original.size() - 10, 'x') + ".dll";
        const auto found = std::search(bytes.begin(), bytes.end(), original.begin(), original.end());
        if (found == bytes.end()) return 20;
        std::copy(renamed.begin(), renamed.end(), found);
        std::ofstream output(missing_cudnn, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const auto partial = base / L"partial-runtime";
    std::filesystem::create_directories(partial);
    std::filesystem::copy_file(base / L"good" / fixture, partial / fixture, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(base / L"good/ggml-test-dependency.dll", partial / L"ggml-test-dependency.dll", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(base / L"good/ggml-test-dependency.dll", partial / L"cudnn64_9.dll", std::filesystem::copy_options::overwrite_existing);
    int failures = 0;
    for (const std::wstring mode : {L"bad", L"good", L"loaded-conflict", L"missing", L"invalid", L"missing-cudnn", L"optional-owner", L"partial-owner"})
    {
        const auto marker = base / (mode + L".marker");
        std::error_code code;
        std::filesystem::remove(marker, code);
        const auto target = mode == L"bad" ? base / L"bad" / fixture : mode == L"missing" ? base / L"missing.dll" :
                            mode == L"invalid" ? invalid : mode == L"missing-cudnn" ? missing_cudnn : mode == L"partial-owner" ? partial / fixture : base / L"good" / fixture;
        std::wstring command = argument(own.wstring()) + L" --child " + argument(mode) + L" " +
            argument(target.wstring()) + L" " + argument(marker.wstring()) + L" " + argument(old.wstring());
        STARTUPINFOW startup{sizeof(STARTUPINFOW)};
        PROCESS_INFORMATION process{};
        bool ok = CreateProcessW(own.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, base.c_str(), &startup, &process) != FALSE;
        if (ok)
        {
            CloseHandle(process.hThread);
            const DWORD waited = WaitForSingleObject(process.hProcess, 10000);
            if (waited != WAIT_OBJECT_0)
            {
                TerminateProcess(process.hProcess, 99);
                WaitForSingleObject(process.hProcess, 1000);
                ok = false;
            }
            DWORD exit = 1;
            if (!GetExitCodeProcess(process.hProcess, &exit) || exit != 0) ok = false;
            CloseHandle(process.hProcess);
        }
        const auto utf8 = std::filesystem::path(mode).u8string();
        report(std::string(ok ? "PASS " : "FAIL ") + std::string(utf8.begin(), utf8.end()) + " loader child finishes within 10 seconds\n");
        if (!ok) ++failures;
        std::filesystem::remove(marker, code);
    }
    std::error_code code;
    std::filesystem::remove(invalid, code);
    std::filesystem::remove(missing_cudnn, code);
    std::filesystem::remove(partial / fixture, code);
    std::filesystem::remove(partial / L"ggml-test-dependency.dll", code);
    std::filesystem::remove(partial / L"cudnn64_9.dll", code);
    std::filesystem::remove(partial, code);
    return failures == 0 ? 0 : 1;
}
