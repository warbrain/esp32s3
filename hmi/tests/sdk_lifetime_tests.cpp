#include "engine/native_client.hpp"
#include "base/logger.hpp"
#include "environment.hpp"
#include <audiocpp.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <stdexcept>

namespace
{
class ScopedEnvironment
{
public:
    explicit ScopedEnvironment(const wchar_t *name) : name_(name)
    {
        wchar_t *value = nullptr;
        std::size_t size = 0;
        if (_wdupenv_s(&value, &size, name) == 0 && value)
            previous_ = value;
        std::free(value);
    }

    ~ScopedEnvironment() { _wputenv_s(name_.c_str(), previous_.c_str()); }
    bool set(const std::wstring &value) const { return _wputenv_s(name_.c_str(), value.c_str()) == 0; }

private:
    std::wstring name_;
    std::wstring previous_;
};
}

int wmain(int argc, wchar_t **argv)
{
    if (!hmi_test::initialize_environment()) return 1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2)
    {
        std::printf("usage: sdk_lifetime_tests <fixture.dll>\n");
        return 2;
    }
    int failures = 0;
    const auto check = [&](bool value, const char *description) {
        std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
        if (!value) ++failures;
    };

    std::error_code code;
    const auto fixture = std::filesystem::absolute(argv[1], code);
    if (code || !std::filesystem::is_regular_file(fixture, code))
    {
        std::printf("fixture DLL does not exist\n");
        return 2;
    }
    const auto temporary = std::filesystem::temp_directory_path() /
        (L"hmi-sdk-lifetime-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(temporary, code);
    if (code) return 2;
    const auto config = temporary / "engine.json";
    {
        std::ofstream file(config, std::ios::binary | std::ios::trunc);
        file << "{\"families\":\"qwen3_asr\"}\n";
        if (!file) return 2;
    }

    const ScopedEnvironment dll_environment(L"HMI_NATIVE_DLL");
    const ScopedEnvironment config_environment(L"HMI_NATIVE_CONFIG");
    const ScopedEnvironment legacy_config_environment(L"AUDIOCPP_CONFIG");
    const ScopedEnvironment failure_environment(L"HMI_TEST_SDK_FAILURE");
    check(dll_environment.set(fixture.wstring()), "fixture SDK path configured");
    check(config_environment.set(config.wstring()), "isolated config configured");
    const auto legacy_obstacle = temporary / "legacy-config-obstacle";
    { std::ofstream file(legacy_obstacle, std::ios::binary); file << "must remain a file"; }
    check(legacy_config_environment.set((legacy_obstacle / "engine.json").wstring()),
        "unusable legacy config configured to check native precedence");
    check(failure_environment.set(L""), "fixture starts in success mode");
    {
        hmi::base::Logger logger;
        hmi::base::LoggerOptions logging;
        logging.file = temporary / "diagnostics-compatibility.log";
        std::string logging_error;
        check(logger.configure(logging, &logging_error), "compatibility diagnostic log configured");
        hmi::engine::Client client;
        const auto closed_queries = [&](const char *description) {
            check(!client.ready() && client.registry() == nullptr && client.families().empty() &&
                client.tasks().empty() && client.last_error().empty(), description);
            (void)client.version();
            check(GetModuleHandleW(fixture.c_str()) == nullptr, "fixture DLL is actually unloaded");
        };
        closed_queries("queries before open return empty state");

        std::string error;
        std::vector<hmi::engine::SdkInitializationStage> stages;
        const auto observe = [&](const hmi::engine::SdkInitializationStage &stage) { stages.push_back(stage); };
        check(client.open(&error, observe), "fixture SDK opens successfully");
        bool paired_stages = stages.size() == 18;
        for (std::size_t index = 0; index + 1 < stages.size(); index += 2)
            paired_stages = paired_stages && !stages[index].completed && stages[index + 1].completed &&
                stages[index].name == stages[index + 1].name && stages[index + 1].success &&
                stages[index + 1].elapsed_ms >= 0.0;
        check(paired_stages && stages.back().name == "registry_and_backends",
            "all nine SDK stages publish paired begin/end timing");
        check(client.ready() && client.registry() != nullptr, "open creates a ready registry");
        constexpr std::uint32_t expected_abi = (AUDIOCPP_ABI_VERSION_MAJOR << 16) |
            (AUDIOCPP_ABI_VERSION_MINOR << 8) | AUDIOCPP_ABI_VERSION_PATCH;
        check(client.version().abi == expected_abi && client.version().build == "lifetime-fixture",
            "version is copied from the loaded DLL");
        check(client.tasks() == std::vector<std::string>{"asr"}, "task query calls loaded DLL");
        check(client.families() == std::vector<std::string>{"qwen3_asr"}, "family query calls loaded DLL");
        check(client.last_error() == "fixture registry failure", "error query calls loaded DLL");
        client.close();
        closed_queries("queries after close never call unloaded exports");
        check(client.version().build == "lifetime-fixture", "copied version survives DLL unload");
        client.close();
        closed_queries("repeated close remains safe");

        check(client.open(&error, {}, &logger), "missing optional diagnostic exports do not prevent SDK open");
        client.close();
        logger.flush();
        {
            std::ifstream file(logging.file, std::ios::binary);
            const std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            check(content.find("event=native_diagnostics timing=unavailable") != std::string::npos,
                "missing optional diagnostics are reported explicitly");
        }
        closed_queries("diagnostic fallback closes without calling missing exports");

        check(config_environment.set(L""), "native config override cleared for compatibility check");
        check(legacy_config_environment.set(config.wstring()), "valid legacy config selected");
        check(client.open(&error), "legacy configuration override remains compatible");
        client.close();
        check(config_environment.set(config.wstring()), "native configuration override restored");

        check(failure_environment.set(L"abi"), "fixture selects ABI rejection");
        check(!client.open(&error) && error.find("ABI mismatch") != std::string::npos,
            "ABI rejection retains caller error text");
        closed_queries("queries after ABI rejection remain safe");

        check(failure_environment.set(L"registry"), "fixture selects registry rejection");
        stages.clear();
        check(!client.open(&error, observe) && error.find("fixture registry failure") != std::string::npos,
            "registry rejection copies SDK detail before unload");
        check(!stages.empty() && stages.back().name == "registry_and_backends" &&
            stages.back().completed && !stages.back().success &&
            stages.back().detail.find("fixture registry failure") != std::string::npos,
            "failed SDK stage reports its duration and preserved detail");
        closed_queries("queries after registry rejection remain safe");

        check(failure_environment.set(L""), "fixture returns to success mode");
        check(client.open(&error, [](const hmi::engine::SdkInitializationStage &) {
            SetLastError(ERROR_INVALID_DATA);
            throw std::runtime_error("observer failure");
        }), "observer exceptions do not prevent SDK initialization");
        check(client.open(&error), "open safely replaces an already open SDK");
        client.close();
        closed_queries("queries after reopen and close remain safe");
        check(dll_environment.set((fixture.parent_path() / "missing-lifetime-sdk.dll").wstring()),
            "missing explicit SDK path configured");
        check(!client.open(&error) && !error.empty(), "missing SDK reports an error");
        closed_queries("queries after missing DLL remain safe");
        check(dll_environment.set(fixture.wstring()), "fixture SDK path restored");
        check(client.open(&error), "client recovers after missing DLL");
    }
    check(GetModuleHandleW(fixture.c_str()) == nullptr, "destructor unloads the final open SDK");
    std::filesystem::remove_all(temporary, code);
    check(!code, "isolated config cleaned up");
    std::printf("%d failure(s); no models, recording, input, or playback\n", failures);
    return failures == 0 ? 0 : 1;
}
