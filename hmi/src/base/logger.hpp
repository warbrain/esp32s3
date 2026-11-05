#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace hmi::base
{
enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off
};

struct LoggerOptions
{
    std::filesystem::path file = "logs/hmi.log";
    LogLevel level = LogLevel::Info;
    std::size_t max_bytes = 5 * 1024 * 1024;
    std::size_t max_files = 3;
    bool console = false;
};

struct LoggerStatistics
{
    std::uint64_t write_calls = 0;
    std::uint64_t emitted_messages = 0; // Submitted to sinks; failures are counted separately.
    std::uint64_t coalesced_messages = 0;
    std::uint64_t filtered_messages = 0;
    std::uint64_t failures = 0;
    std::uint64_t lock_wait_ns = 0; // Included in write_ns, not an additional cost.
    std::uint64_t write_ns = 0; // write()/write_repeated(), including lock wait and their sink flushes.
    std::uint64_t max_write_ns = 0;
    std::uint64_t flush_calls = 0; // Explicit flush() requests, including requests without an open sink.
    std::uint64_t flush_lock_wait_ns = 0; // Included in flush_ns.
    std::uint64_t flush_ns = 0; // Explicit flush() calls only, including lock wait and repeat emission.
    std::uint64_t max_flush_ns = 0;
    std::uint64_t sink_write_calls = 0;
    std::uint64_t sink_write_ns = 0; // All logger submissions, including repeat and statistics records.
    std::uint64_t sink_flush_calls = 0;
    std::uint64_t sink_flush_ns = 0; // All explicit sink flushes, including errors/reconfigure/shutdown.
    std::uint64_t write_sink_flush_ns = 0; // Subset of both write_ns and sink_flush_ns.
};

bool parse_log_level(std::string_view text, LogLevel &out) noexcept;
std::string_view log_level_name(LogLevel level) noexcept;

// Own one logger per output file. No instance enters spdlog's global registry.
// Writes and reconfiguration are serialized, including when reopening the same
// file. Error/critical entries flush immediately; destruction flushes all levels.
class Logger
{
public:
    Logger();
    ~Logger();
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    // Relative paths are resolved at configuration time. Rotation accepts
    // 1 KiB..1 GiB and 1..100 backup files. A single entry can exceed the
    // rotation threshold. A failed configuration retains the current logger.
    bool configure(const LoggerOptions &options, std::string *error = nullptr);

    // Text is data, never a format string. Control characters are escaped so
    // one call produces one event line. Failures are retained in last_error().
    void write(LogLevel level, std::string_view module, std::string_view message) noexcept;
    // Opt-in exact consecutive repetition aggregation. Include request IDs in
    // event messages. Warnings/errors are always emitted individually.
    void write_repeated(LogLevel level, std::string_view module, std::string_view message) noexcept;
    LoggerStatistics statistics() const noexcept;
    // Flushes prior records, then writes a detached statistics snapshot. The
    // snapshot excludes its own submission and subsequent shutdown work.
    void write_statistics() noexcept;
    void flush() noexcept;
    void shutdown() noexcept;

    bool ready() const noexcept;
    LoggerOptions options() const;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
