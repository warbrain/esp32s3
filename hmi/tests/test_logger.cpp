#include "check.hpp"
#include "base/logger.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
class LogDirectory
{
public:
    LogDirectory()
    {
        parent_ = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = parent_ / (L"hmi-logger-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(stamp));
            std::error_code code;
            if (std::filesystem::create_directory(path, code)) return;
        }
        throw std::runtime_error("cannot create isolated logger fixture");
    }

    ~LogDirectory()
    {
        const auto checked = std::filesystem::absolute(path).lexically_normal();
        // Delete only this fixture's own uniquely named directory.
        if (checked.parent_path() == parent_ && checked.filename().wstring().starts_with(L"hmi-logger-"))
        {
            std::error_code code;
            std::filesystem::remove_all(checked, code);
        }
    }

    std::filesystem::path path;

private:
    std::filesystem::path parent_;
};

std::string read_log(const std::filesystem::path &file)
{
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

HMI_TEST(logger_unicode_paths_filtering_and_literal_messages)
{
    LogDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions options;
    options.file = directory.path / L"基础日志" / L"运行.log";
    options.level = hmi::base::LogLevel::Warning;
    std::string error = "stale";
    CHECK(logger.configure(options, &error));
    CHECK(error.empty());
    CHECK(logger.ready());
    CHECK(logger.options().file == options.file);
    logger.write(hmi::base::LogLevel::Info, "test", "filtered-marker");
    logger.write(hmi::base::LogLevel::Warning, "模块", "保留消息 {bad-format} %s");
    logger.write(hmi::base::LogLevel::Warning, "module\nnext", std::string_view("line\r\nnext\0tail", 15));
    logger.flush();
    const auto text = read_log(options.file);
    CHECK(text.find("filtered-marker") == std::string::npos);
    CHECK(text.find("[模块] 保留消息 {bad-format} %s") != std::string::npos);
    CHECK(text.find("[module\\nnext] line\\r\\nnext\\x00tail") != std::string::npos);
    CHECK(std::count(text.begin(), text.end(), '\n') == 2);
    logger.shutdown();
    CHECK(!logger.ready());
    logger.write(hmi::base::LogLevel::Error, "closed", "must not reopen");
    CHECK(read_log(options.file) == text);
}

HMI_TEST(logger_error_entries_flush_without_explicit_flush)
{
    LogDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions options;
    options.file = directory.path / "errors.log";
    CHECK(logger.configure(options));
    logger.write(hmi::base::LogLevel::Error, "engine", "immediate-error-marker");
    CHECK(read_log(options.file).find("immediate-error-marker") != std::string::npos);
}

HMI_TEST(logger_rotation_keeps_bounded_backups_and_recent_entries)
{
    LogDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions options;
    options.file = directory.path / "rotate.log";
    options.max_bytes = 1024;
    options.max_files = 2;
    CHECK(logger.configure(options));
    for (int index = 0; index < 120; ++index)
        logger.write(hmi::base::LogLevel::Info, "rotate", "entry=" + std::to_string(index) + " " + std::string(100, 'x'));
    logger.write(hmi::base::LogLevel::Error, "rotate", "tail-marker");
    logger.shutdown();
    std::size_t files = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory.path))
    {
        if (entry.path().extension() != ".log") continue;
        ++files;
        CHECK(entry.file_size() <= options.max_bytes);
    }
    CHECK(files == 3);
    CHECK(std::filesystem::exists(directory.path / "rotate.1.log"));
    CHECK(std::filesystem::exists(directory.path / "rotate.2.log"));
    CHECK(!std::filesystem::exists(directory.path / "rotate.3.log"));
    CHECK(read_log(options.file).find("tail-marker") != std::string::npos);
}

HMI_TEST(logger_failed_reconfigure_preserves_previous_file)
{
    LogDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions options;
    options.file = directory.path / "working.log";
    CHECK(logger.configure(options));
    logger.write(hmi::base::LogLevel::Info, "test", "before-reconfigure");
    const auto obstacle = directory.path / "not-a-directory";
    { std::ofstream output(obstacle); output << "original-file"; }
    auto invalid = options;
    invalid.file = obstacle / "rejected.log";
    std::string error;
    CHECK(!logger.configure(invalid, &error));
    CHECK(!error.empty());
    CHECK(!logger.last_error().empty());
    CHECK(logger.ready());
    CHECK(logger.options().file == options.file);
    logger.write(hmi::base::LogLevel::Error, "test", "after-reconfigure");
    const auto text = read_log(options.file);
    CHECK(text.find("before-reconfigure") != std::string::npos);
    CHECK(text.find("after-reconfigure") != std::string::npos);
    CHECK(read_log(obstacle) == "original-file");
    const auto victim = directory.path / L"must-not-change.yaml";
    { std::ofstream output(victim, std::ios::binary); output << "schema: 1\n"; }
    auto nul_path = victim.native();
    nul_path.push_back(std::filesystem::path::value_type{});
    nul_path += L"ignored.log";
    invalid = options;
    invalid.file = std::filesystem::path(nul_path);
    CHECK(!logger.configure(invalid, &error));
    CHECK(error.find("NUL") != std::string::npos);
    CHECK(read_log(victim) == "schema: 1\n");
    CHECK(logger.options().file == options.file);
    invalid = options;
    invalid.max_bytes = 0;
    CHECK(!logger.configure(invalid));
    invalid = options;
    invalid.max_files = 0;
    CHECK(!logger.configure(invalid));
    invalid = options;
    invalid.level = static_cast<hmi::base::LogLevel>(99);
    CHECK(!logger.configure(invalid));
    CHECK(logger.configure(options, &error));
    CHECK(error.empty());
    CHECK(logger.last_error().empty());
}

HMI_TEST(logger_parallel_writes_survive_same_file_reconfiguration)
{
    LogDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions options;
    options.file = directory.path / "parallel.log";
    CHECK(logger.configure(options));
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 6; ++worker)
        workers.emplace_back([&, worker] {
            for (int entry = 0; entry < 100; ++entry)
                logger.write(hmi::base::LogLevel::Info, "parallel",
                    "worker=" + std::to_string(worker) + " entry=" + std::to_string(entry));
        });
    for (int index = 0; index < 8; ++index) CHECK(logger.configure(options));
    for (auto &worker : workers) worker.join();
    logger.flush();
    const auto text = read_log(options.file);
    CHECK(std::count(text.begin(), text.end(), '\n') == 600);
    for (int worker = 0; worker < 6; ++worker)
        for (int entry = 0; entry < 100; ++entry)
        {
            const auto marker = "[parallel] worker=" + std::to_string(worker) + " entry=" + std::to_string(entry) + "\n";
            // Windows spdlog emits CRLF; check the message independently of EOL.
            const auto body = marker.substr(0, marker.size() - 1);
            CHECK(text.find(body + "\n") != std::string::npos || text.find(body + "\r\n") != std::string::npos);
        }
}

HMI_TEST(logger_instances_are_independent_and_levels_parse)
{
    LogDirectory directory;
    hmi::base::Logger first;
    hmi::base::Logger second;
    hmi::base::LoggerOptions options;
    options.file = directory.path / "first.log";
    CHECK(first.configure(options));
    options.file = directory.path / "second.log";
    CHECK(second.configure(options));
    first.write(hmi::base::LogLevel::Error, "first", "first-only");
    second.write(hmi::base::LogLevel::Critical, "second", "second-only");
    CHECK(read_log(directory.path / "first.log").find("second-only") == std::string::npos);
    CHECK(read_log(directory.path / "second.log").find("first-only") == std::string::npos);
    CHECK(read_log(directory.path / "first.log").find("first-only") != std::string::npos);
    CHECK(read_log(directory.path / "second.log").find("second-only") != std::string::npos);
    hmi::base::LogLevel level = hmi::base::LogLevel::Off;
    CHECK(hmi::base::parse_log_level("WARNING", level));
    CHECK(level == hmi::base::LogLevel::Warning);
    CHECK(hmi::base::parse_log_level("warn", level));
    CHECK(!hmi::base::parse_log_level("warn ", level));
    CHECK(level == hmi::base::LogLevel::Warning);
    CHECK(hmi::base::log_level_name(level) == "warning");
    CHECK(hmi::base::log_level_name(static_cast<hmi::base::LogLevel>(99)) == "invalid");
}

HMI_TEST(logger_explicit_repeat_aggregation_flushes_counts_without_hiding_failures_or_requests)
{
    LogDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions options;
    options.file = directory.path / "repeated.log";
    CHECK(logger.configure(options));
    for (int index = 0; index < 5; ++index)
        logger.write_repeated(hmi::base::LogLevel::Info, "capture", "event=idle request_id=1");
    logger.write_repeated(hmi::base::LogLevel::Warning, "capture", "same-warning");
    logger.write_repeated(hmi::base::LogLevel::Warning, "capture", "same-warning");
    logger.write_repeated(hmi::base::LogLevel::Error, "capture", "same-error");
    logger.write_repeated(hmi::base::LogLevel::Error, "capture", "same-error");
    logger.write_repeated(hmi::base::LogLevel::Info, "action", "event=complete request_id=2");
    logger.write_repeated(hmi::base::LogLevel::Info, "action", "event=complete request_id=3");
    logger.flush();
    auto text = read_log(options.file);
    CHECK(text.find("repeat_count=4 first_repeat_unix_ms=") != std::string::npos);
    CHECK(text.find("last_repeat_unix_ms=") != std::string::npos);
    CHECK(text.find("request_id=2") != std::string::npos && text.find("request_id=3") != std::string::npos);
    const auto occurrences = [](const std::string &body, const std::string &marker) {
        std::size_t count = 0, cursor = 0;
        while ((cursor = body.find(marker, cursor)) != std::string::npos) { ++count; cursor += marker.size(); }
        return count;
    };
    CHECK(occurrences(text, "same-warning") == 2 && occurrences(text, "same-error") == 2);
    const auto statistics = logger.statistics();
    CHECK(statistics.write_calls == 11 && statistics.coalesced_messages == 4);
    CHECK(statistics.failures == 0 && statistics.flush_calls == 1);
    CHECK(statistics.emitted_messages == 8);
    CHECK(statistics.write_ns >= statistics.max_write_ns && statistics.write_ns >= statistics.lock_wait_ns);
    logger.write_repeated(hmi::base::LogLevel::Info, "tail", "flush-marker");
    logger.write_repeated(hmi::base::LogLevel::Info, "tail", "flush-marker");
    logger.flush();
    CHECK(read_log(options.file).find("flush-marker repeat_count=1") != std::string::npos);
    logger.write_repeated(hmi::base::LogLevel::Info, "tail", "reconfigure-marker");
    logger.write_repeated(hmi::base::LogLevel::Info, "tail", "reconfigure-marker");
    CHECK(logger.configure(options));
    CHECK(read_log(options.file).find("reconfigure-marker repeat_count=1") != std::string::npos);
    logger.write_repeated(hmi::base::LogLevel::Info, "tail", "shutdown-marker");
    logger.write_repeated(hmi::base::LogLevel::Info, "tail", "shutdown-marker");
    logger.shutdown();
    CHECK(read_log(options.file).find("shutdown-marker repeat_count=1") != std::string::npos);
}

HMI_TEST(logger_default_writes_remain_uncoalesced_and_summary_is_explicit)
{
    LogDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions options;
    options.file = directory.path / "ordinary.log";
    CHECK(logger.configure(options));
    for (int index = 0; index < 3; ++index)
        logger.write(hmi::base::LogLevel::Info, "action", "same-action");
    logger.flush();
    auto text = read_log(options.file);
    CHECK(std::count(text.begin(), text.end(), '\n') == 3);
    CHECK(text.find("coalesced_messages=") == std::string::npos);
    logger.write_statistics();
    logger.flush();
    text = read_log(options.file);
    CHECK(text.find("[logger] event=summary write_calls=3") != std::string::npos);
    CHECK(text.find("coalesced_messages=0") != std::string::npos);
    CHECK(text.find("lock_wait_ns=") != std::string::npos && text.find("max_write_ns=") != std::string::npos);
    CHECK(text.find("write_scope=write_calls_including_lock_wait_and_sink_flush") != std::string::npos);
    CHECK(text.find("flush_scope=explicit_flush_calls_including_lock_wait") != std::string::npos);
    CHECK(text.find("sink_write_excludes_sink_flush=1 counters_overlap=1") != std::string::npos);
    CHECK(text.find("caller_formatting=excluded native_logging=separate later_shutdown=excluded") != std::string::npos);
    logger.write_repeated(hmi::base::LogLevel::Info, "escape", "line\nnext");
    logger.write_repeated(hmi::base::LogLevel::Info, "escape", "line\\nnext");
    logger.flush();
    CHECK(logger.statistics().coalesced_messages == 0);
}

HMI_TEST(logger_measures_error_flush_explicit_flush_and_lifecycle_flush_separately)
{
    LogDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions options;
    options.file = directory.path / "flush-cost.log";
    CHECK(logger.configure(options));
    logger.write(hmi::base::LogLevel::Info, "test", "buffered");
    auto value = logger.statistics();
    CHECK(value.sink_write_calls == 1 && value.sink_flush_calls == 0 && value.flush_calls == 0);
    logger.write(hmi::base::LogLevel::Error, "test", "immediate-error");
    const auto after_error = logger.statistics();
    CHECK(read_log(options.file).find("immediate-error") != std::string::npos);
    CHECK(after_error.sink_write_calls == 2 && after_error.sink_flush_calls == 1);
    CHECK(after_error.sink_flush_ns > 0 && after_error.sink_flush_ns == after_error.write_sink_flush_ns);
    CHECK(after_error.write_ns >= after_error.write_sink_flush_ns && after_error.flush_ns == 0);

    logger.flush();
    const auto after_flush = logger.statistics();
    CHECK(after_flush.flush_calls == 1 && after_flush.sink_flush_calls == 2);
    CHECK(after_flush.write_calls == after_error.write_calls && after_flush.write_ns == after_error.write_ns);
    CHECK(after_flush.sink_write_ns == after_error.sink_write_ns);
    CHECK(after_flush.write_sink_flush_ns == after_error.write_sink_flush_ns);
    CHECK(after_flush.flush_ns >= after_flush.flush_lock_wait_ns && after_flush.flush_ns >= after_flush.max_flush_ns);
    CHECK(after_flush.flush_ns >= after_flush.sink_flush_ns - after_error.sink_flush_ns);

    CHECK(logger.configure(options));
    value = logger.statistics();
    CHECK(value.sink_flush_calls == 3 && value.flush_calls == 1 && value.write_ns == after_error.write_ns);
    logger.write_statistics();
    value = logger.statistics();
    CHECK(value.sink_flush_calls == 4 && value.sink_write_calls == 3 && value.flush_calls == 1);
    logger.shutdown();
    value = logger.statistics();
    CHECK(value.sink_flush_calls == 5 && value.flush_calls == 1 && value.write_ns == after_error.write_ns);
    CHECK(read_log(options.file).find("sink_write_calls=2") != std::string::npos);
    CHECK(read_log(options.file).find("sink_flush_calls=4") != std::string::npos);
    logger.flush();
    value = logger.statistics();
    CHECK(value.flush_calls == 2 && value.sink_flush_calls == 5);
}
