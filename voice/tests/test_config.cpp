#include "check.hpp"
#include "base/config.hpp"
#include <yaml-cpp/fptostring.h>

#include <windows.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct ConfigDirectory
{
    std::filesystem::path parent = std::filesystem::temp_directory_path();
    std::filesystem::path path = parent / (L"hmi-config-中文-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    ConfigDirectory() { std::filesystem::create_directories(path); }
    ~ConfigDirectory()
    {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        const auto base = std::filesystem::weakly_canonical(parent, error);
        if (!error && resolved.parent_path() == base && resolved.filename().native().starts_with(L"hmi-config-"))
            std::filesystem::remove_all(resolved, error);
    }
};

void write_file(const std::filesystem::path &file, const std::string &text)
{
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string read_file(const std::filesystem::path &file)
{
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

HMI_TEST(config_yaml_unicode_nested_roundtrip)
{
    ConfigDirectory directory;
    const auto file = directory.path / L"配置" / L"应用.yaml";
    hmi::base::Config written;
    std::string error = "previous error";
    CHECK(written.set("voice.backend", "cuda", &error));
    CHECK(error.empty());
    CHECK(written.set("voice.threads", 4));
    CHECK(written.set("voice.enabled", true));
    CHECK(written.set("display.title", std::string("你好，配置")));
    CHECK(written.set("model.names", std::vector<std::string>{"asr", "tts"}));
    CHECK(written.save(file, &error));
    CHECK(error.empty());
    hmi::base::Config loaded;
    CHECK(loaded.load(file, &error));
    CHECK(error.empty());
    CHECK(loaded.get<std::string>("voice.backend", "none") == "cuda");
    CHECK(loaded.get<int>("voice.threads", 1) == 4);
    CHECK(loaded.get<bool>("voice.enabled", false));
    CHECK(loaded.get<std::string>("display.title", "") == "你好，配置");
    CHECK((loaded.get<std::vector<std::string>>("model.names", {}) == std::vector<std::string>{"asr", "tts"}));
    CHECK(loaded.get<int>("absent.path", 9) == 9);
    CHECK(!loaded.value("missing", &error).IsDefined());
    CHECK(error.empty());
}

HMI_TEST(config_invalid_reload_preserves_last_good_values)
{
    ConfigDirectory directory;
    const auto file = directory.path / L"app.yaml";
    hmi::base::Config config;
    CHECK(config.set("voice.threads", 6));
    std::string error;
    for (const auto &invalid : {"voice: [unterminated", "voice: 1\nvoice: 2\n", "root: &loop [*loop]\n", "- sequence-root\n"})
    {
        write_file(file, invalid);
        CHECK(!config.load(file, &error));
        CHECK(!error.empty());
        CHECK(config.get<int>("voice.threads", 0) == 6);
    }
    CHECK(!config.load(directory.path / L"missing.yaml", &error));
    CHECK(config.get<int>("voice.threads", 0) == 6);
}

HMI_TEST(config_type_errors_and_invalid_paths_are_transactional)
{
    hmi::base::Config config;
    CHECK(config.set("voice.backend", "cuda"));
    std::string error;
    int number = 17;
    CHECK(!config.try_get("voice.backend", number, &error));
    CHECK(number == 17);
    CHECK(!error.empty());
    CHECK(config.get<int>("voice.backend", 23) == 23);
    CHECK(!config.try_get("missing", number, &error));
    CHECK(number == 17);
    for (const std::string_view invalid : {"", ".voice", "voice.", "voice..backend"})
    {
        CHECK(!config.set(invalid, 99, &error));
        CHECK(!error.empty());
        CHECK(!config.value(invalid, &error).IsDefined());
        CHECK(!error.empty());
    }
    CHECK(!config.set("voice.backend.child", 1, &error));
    CHECK(config.get<std::string>("voice.backend", "") == "cuda");
    CHECK(!config.set_node("undefined", YAML::Node(YAML::NodeType::Undefined), &error));
    CHECK(!config.value("undefined").IsDefined());
}

HMI_TEST(config_snapshots_and_alias_updates_are_independent)
{
    ConfigDirectory directory;
    const auto file = directory.path / L"aliases.yaml";
    write_file(file, "first: &shared {threads: 2}\nsecond: *shared\n");
    hmi::base::Config config;
    CHECK(config.load(file));
    auto snapshot = config.snapshot();
    snapshot["first"]["threads"] = 50;
    auto value = config.value("second");
    value["threads"] = 60;
    CHECK(config.get<int>("first.threads", 0) == 2);
    CHECK(config.get<int>("second.threads", 0) == 2);
    CHECK(config.set("first.threads", 7));
    CHECK(config.get<int>("first.threads", 0) == 7);
    CHECK(config.get<int>("second.threads", 0) == 2);
}

HMI_TEST(config_rejects_oversized_and_deep_documents)
{
    ConfigDirectory directory;
    const auto file = directory.path / L"large.yaml";
    hmi::base::Config config;
    CHECK(config.set("safe", true));
    write_file(file, "large: " + std::string(4 * 1024 * 1024, 'x'));
    std::string error;
    CHECK(!config.load(file, &error));
    CHECK(config.get<bool>("safe", false));
    std::string nested;
    for (int level = 0; level < 70; ++level) nested += std::string(static_cast<std::size_t>(level * 2), ' ') + "item:\n";
    write_file(file, nested);
    CHECK(!config.load(file, &error));
    CHECK(config.get<bool>("safe", false));
    std::string expansion = "a: &a [value,value,value,value,value,value,value,value]\n";
    for (char key = 'b'; key <= 'g'; ++key)
    {
        expansion += std::string(1, key) + ": &" + key + " [";
        for (int repeat = 0; repeat < 8; ++repeat)
            expansion += (repeat ? ",*" : "*") + std::string(1, static_cast<char>(key - 1));
        expansion += "]\n";
    }
    write_file(file, expansion);
    CHECK(!config.load(file, &error));
    CHECK(config.get<bool>("safe", false));
}

HMI_TEST(config_failed_atomic_replace_preserves_file_and_cleans_temporary)
{
    ConfigDirectory directory;
    const auto file = directory.path / L"locked.yaml";
    write_file(file, "original: true\n");
    const std::string original = read_file(file);
    hmi::base::Config config;
    CHECK(config.set("replacement", 42));
    const HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE);
    std::string error;
    CHECK(!config.save(file, &error));
    CHECK(!error.empty());
    CHECK(read_file(file) == original);
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    std::size_t files = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory.path))
    {
        ++files;
        CHECK(entry.path() == file);
    }
    CHECK(files == 1);
    CHECK(config.save(file, &error));
    hmi::base::Config reloaded;
    CHECK(reloaded.load(file));
    CHECK(reloaded.get<int>("replacement", 0) == 42);
}

HMI_TEST(config_concurrent_reads_writes_and_saves_keep_complete_documents)
{
    ConfigDirectory directory;
    const auto file = directory.path / L"parallel.yaml";
    hmi::base::Config config;
    CHECK(config.set("counter", 0));
    std::atomic<bool> valid{true};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 3; ++worker)
        workers.emplace_back([&, worker] {
            for (int iteration = 0; iteration < 25; ++iteration)
            {
                if (!config.set("counter", worker * 100 + iteration)) valid.store(false);
                int number = -1;
                if (!config.try_get("counter", number) || number < 0 || number > 224) valid.store(false);
                auto copy = config.snapshot();
                copy["counter"] = -100;
                if (!config.save(file)) valid.store(false);
            }
        });
    for (auto &worker : workers) worker.join();
    CHECK(valid.load());
    hmi::base::Config read;
    CHECK(read.load(file));
    CHECK(read.get<int>("counter", -1) >= 0);
}

HMI_TEST(config_yaml_float_formatting_preserves_extremes_and_bounds_precision)
{
    const auto check_roundtrip = []<typename T>(T value, std::size_t precision) {
        const auto text = YAML::FpToString(value, precision);
        T parsed{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        CHECK(result.ec == std::errc{});
        CHECK(result.ptr == text.data() + text.size());
        CHECK(parsed == value);
    };
    for (const auto value : {0.0F, 1.25F, std::numeric_limits<float>::max(),
            std::numeric_limits<float>::lowest(), std::numeric_limits<float>::min(),
            std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::epsilon()})
        check_roundtrip(value, static_cast<std::size_t>(std::numeric_limits<float>::max_digits10));
    for (const auto value : {0.0, 1.25, std::numeric_limits<double>::max(),
            std::numeric_limits<double>::lowest(), std::numeric_limits<double>::min(),
            std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::epsilon()})
        check_roundtrip(value, static_cast<std::size_t>(std::numeric_limits<double>::max_digits10));

    const auto oversized_precision = std::numeric_limits<std::size_t>::max();
    const auto bounded_precision = static_cast<std::size_t>(std::numeric_limits<int>::max());
    for (const auto value : {1.25F, std::numeric_limits<float>::max()})
    {
        CHECK(YAML::FpToString(value, oversized_precision) == YAML::FpToString(value, bounded_precision));
        check_roundtrip(value, oversized_precision);
    }
    for (const auto value : {1.25, std::numeric_limits<double>::max()})
    {
        CHECK(YAML::FpToString(value, oversized_precision) == YAML::FpToString(value, bounded_precision));
        check_roundtrip(value, oversized_precision);
    }
}
