#include "check.hpp"
#include "app/voice_runtime.hpp"
#include "intent/rule_io.hpp"

#include <windows.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
struct EnvironmentValue
{
    std::wstring name;
    std::wstring previous;
    bool existed = false;
    EnvironmentValue(const wchar_t *key, const wchar_t *value) : name(key)
    {
        wchar_t *old = nullptr;
        std::size_t count = 0;
        _wdupenv_s(&old, &count, key);
        existed = old != nullptr;
        if (old) previous = old;
        std::free(old);
        _wputenv_s(key, value);
    }
    ~EnvironmentValue() { _wputenv_s(name.c_str(), existed ? previous.c_str() : L""); }
};

struct RulesDirectory
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (L"HMI-中文规则-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    RulesDirectory() { std::filesystem::create_directories(path); }
    ~RulesDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string path_utf8(const std::filesystem::path &path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
}

HMI_TEST(runtime_unicode_rules_load_and_hot_reload)
{
    RulesDirectory directory;
    const auto file = directory.path / L"语音规则.json";
    {
        std::ofstream output(file, std::ios::binary);
        output << hmi::intent::serialize_rules(hmi::intent::default_rules());
    }
    EnvironmentValue rules(L"HMI_RULES", file.c_str());
    // The worker can only observe this missing DLL, so it cannot load models,
    // start capture, register shortcuts, play audio, or execute input actions.
    const auto missing = directory.path / L"missing-sdk.dll";
    EnvironmentValue sdk(L"HMI_NATIVE_DLL", missing.c_str());
    hmi::app::VoiceRuntime runtime;
    std::string error;
    CHECK(runtime.start(&error));
    CHECK(runtime.rules_path() == path_utf8(file));
    runtime.stop();
    const auto original_time = std::filesystem::last_write_time(file);
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << "{invalid-json";
    }
    std::filesystem::last_write_time(file, original_time + std::chrono::seconds(5));
    runtime.tick();
    const auto snapshot = runtime.snapshot();
    CHECK(snapshot.state == hmi::app::VoiceState::Error);
    CHECK(snapshot.status.find("Rules reload rejected:") != std::string::npos);
    CHECK(!runtime.reload_rules(&error));
}
