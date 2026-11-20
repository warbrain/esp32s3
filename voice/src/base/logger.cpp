#include "base/logger.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace hmi::base
{
namespace
{
bool valid_level(LogLevel level) noexcept
{
    return level >= LogLevel::Trace && level <= LogLevel::Off;
}

spdlog::level::level_enum native_level(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Trace: return spdlog::level::trace;
    case LogLevel::Debug: return spdlog::level::debug;
    case LogLevel::Info: return spdlog::level::info;
    case LogLevel::Warning: return spdlog::level::warn;
    case LogLevel::Error: return spdlog::level::err;
    case LogLevel::Critical: return spdlog::level::critical;
    case LogLevel::Off: return spdlog::level::off;
    }
    return spdlog::level::off;
}

std::string path_utf8(const std::filesystem::path &path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

void append_text(std::string &out, std::string_view text)
{
    constexpr char hex[] = "0123456789abcdef";
    for (const char ch : text)
    {
        const auto byte = static_cast<unsigned char>(ch);
        switch (byte)
        {
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (byte < 32 || byte == 127)
            {
                out += "\\x";
                out += hex[byte >> 4];
                out += hex[byte & 15];
            }
            else out += ch;
        }
    }
}

struct ErrorState
{
    mutable std::mutex mutex;
    std::string text;
    std::uint64_t failures = 0;

    void set(std::string_view detail) noexcept
    {
        try
        {
            std::lock_guard lock(mutex);
            text.assign(detail);
            if (!detail.empty()) ++failures;
        }
        catch (...) {}
    }

    std::string get() const
    {
        std::lock_guard lock(mutex);
        return text;
    }

    std::uint64_t count() const
    {
        std::lock_guard lock(mutex);
        return failures;
    }
};
}

struct Logger::Impl
{
    using Clock = std::chrono::steady_clock;
    mutable std::mutex mutex;
    std::shared_ptr<spdlog::logger> logger;
    LoggerOptions options;
    std::shared_ptr<ErrorState> errors = std::make_shared<ErrorState>();
    LoggerStatistics measured;
    std::string previous;
    std::string previous_module;
    std::string previous_message;
    LogLevel previous_level = LogLevel::Off;
    std::thread::id previous_thread;
    std::uint64_t repeats = 0;
    std::int64_t first_repeat_ms = 0;
    std::int64_t last_repeat_ms = 0;
    Clock::time_point repeat_deadline;

    struct Measure
    {
        std::uint64_t &total;
        std::uint64_t *subset = nullptr;
        Clock::time_point started = Clock::now();
        ~Measure()
        {
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
            total += elapsed;
            if (subset) *subset += elapsed;
        }
    };

    void flush_sink(bool within_write = false)
    {
        if (!logger) return;
        ++measured.sink_flush_calls;
        Measure elapsed{measured.sink_flush_ns, within_write ? &measured.write_sink_flush_ns : nullptr};
        logger->flush();
    }

    void emit(LogLevel level, std::string_view text, bool within_write = false)
    {
        ++measured.sink_write_calls;
        {
            Measure elapsed{measured.sink_write_ns};
            logger->log(native_level(level), spdlog::string_view_t(text.data(), text.size()));
        }
        ++measured.emitted_messages;
        if (level >= LogLevel::Error) flush_sink(within_write);
    }

    void flush_repeats(bool within_write = false)
    {
        if (!logger || repeats == 0) return;
        const std::string summary = previous + " repeat_count=" + std::to_string(repeats) +
            " first_repeat_unix_ms=" + std::to_string(first_repeat_ms) +
            " last_repeat_unix_ms=" + std::to_string(last_repeat_ms);
        emit(previous_level, summary, within_write);
        repeats = 0;
        repeat_deadline = Clock::now() + std::chrono::seconds(5);
    }

    void write(LogLevel level, std::string_view module, std::string_view message, bool coalesce) noexcept
    {
        const auto started = Clock::now();
        try
        {
            std::unique_lock lock(mutex);
            const auto acquired = Clock::now();
            ++measured.write_calls;
            measured.lock_wait_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(acquired - started).count());
            struct WriteTime
            {
                LoggerStatistics &measured;
                Clock::time_point started;
                ~WriteTime()
                {
                    const auto elapsed = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
                    measured.write_ns += elapsed;
                    measured.max_write_ns = std::max(measured.max_write_ns, elapsed);
                }
            } elapsed{measured, started};
            if (!valid_level(level) || level == LogLevel::Off || !logger || !logger->should_log(native_level(level)))
            {
                ++measured.filtered_messages;
                return;
            }
            std::string text;
            text.reserve(module.size() + message.size() + 3);
            text += '[';
            append_text(text, module.empty() ? "app" : module);
            text += "] ";
            append_text(text, message);
            coalesce = coalesce && level <= LogLevel::Info;
            const auto thread = std::this_thread::get_id();
            if (coalesce && level == previous_level && thread == previous_thread &&
                module == previous_module && message == previous_message)
            {
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (repeats == 0) first_repeat_ms = now_ms;
                last_repeat_ms = now_ms;
                ++repeats;
                ++measured.coalesced_messages;
                if (acquired >= repeat_deadline) flush_repeats(true);
                return;
            }
            flush_repeats(true);
            emit(level, text, true);
            previous = coalesce ? std::move(text) : std::string{};
            previous_module = coalesce ? std::string(module) : std::string{};
            previous_message = coalesce ? std::string(message) : std::string{};
            previous_level = coalesce ? level : LogLevel::Off;
            previous_thread = thread;
            repeat_deadline = acquired + std::chrono::seconds(5);
        }
        catch (const std::exception &failure) { errors->set(failure.what()); }
        catch (...) { errors->set("unexpected log write failure"); }
    }
};

bool parse_log_level(std::string_view text, LogLevel &out) noexcept
{
    constexpr std::pair<std::string_view, LogLevel> names[]{
        {"trace", LogLevel::Trace}, {"debug", LogLevel::Debug}, {"info", LogLevel::Info},
        {"warning", LogLevel::Warning}, {"warn", LogLevel::Warning},
        {"error", LogLevel::Error}, {"critical", LogLevel::Critical}, {"off", LogLevel::Off}
    };
    for (const auto &[name, value] : names)
    {
        if (text.size() != name.size()) continue;
        bool matches = true;
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            const char ch = text[index] >= 'A' && text[index] <= 'Z' ?
                static_cast<char>(text[index] + ('a' - 'A')) : text[index];
            if (ch != name[index]) { matches = false; break; }
        }
        if (matches) { out = value; return true; }
    }
    return false;
}

std::string_view log_level_name(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    case LogLevel::Critical: return "critical";
    case LogLevel::Off: return "off";
    }
    return "invalid";
}

Logger::Logger() : impl_(std::make_unique<Impl>()) {}
Logger::~Logger() { shutdown(); }

bool Logger::configure(const LoggerOptions &options, std::string *error)
{
    try
    {
        if (!valid_level(options.level)) throw std::runtime_error("invalid log level");
        if (options.max_bytes < 1024 || options.max_bytes > 1024ULL * 1024 * 1024)
            throw std::runtime_error("log rotation max_bytes must be 1024..1073741824");
        if (options.max_files < 1 || options.max_files > 100)
            throw std::runtime_error("log rotation max_files must be 1..100");
        if (options.file.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
            throw std::runtime_error("log file path must not contain NUL");
        if (options.file.empty() || !options.file.has_filename() ||
            options.file.filename() == "." || options.file.filename() == "..")
            throw std::runtime_error("log output must be a file path");

        LoggerOptions resolved = options;
        resolved.file = std::filesystem::absolute(options.file).lexically_normal();
        std::lock_guard lock(impl_->mutex);
        std::error_code code;
        std::filesystem::create_directories(resolved.file.parent_path(), code);
        if (code) throw std::runtime_error("cannot create log directory: " + path_utf8(resolved.file.parent_path()) + ": " + code.message());

        if (impl_->logger) { impl_->flush_repeats(); impl_->flush_sink(); }
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            resolved.file.native(), resolved.max_bytes, resolved.max_files, false));
        if (resolved.console) sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        auto replacement = std::make_shared<spdlog::logger>("hmi", sinks.begin(), sinks.end());
        replacement->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] [thread %t] %v");
        replacement->set_level(native_level(resolved.level));
        // Flush through the measured entry point, preserving immediate errors.
        replacement->flush_on(spdlog::level::off);
        replacement->set_error_handler([state = impl_->errors](const std::string &detail) { state->set(detail); });
        impl_->logger.swap(replacement);
        impl_->previous.clear();
        impl_->previous_module.clear();
        impl_->previous_message.clear();
        impl_->previous_level = LogLevel::Off;
        impl_->options = std::move(resolved);
        impl_->errors->set({});
        if (error) error->clear();
        return true;
    }
    catch (const std::exception &failure)
    {
        impl_->errors->set(failure.what());
        if (error) *error = failure.what();
    }
    catch (...)
    {
        impl_->errors->set("unexpected logger configuration failure");
        if (error) *error = "unexpected logger configuration failure";
    }
    return false;
}

void Logger::write(LogLevel level, std::string_view module, std::string_view message) noexcept
{
    impl_->write(level, module, message, false);
}

void Logger::write_repeated(LogLevel level, std::string_view module, std::string_view message) noexcept
{
    impl_->write(level, module, message, true);
}

LoggerStatistics Logger::statistics() const noexcept
{
    try
    {
        std::lock_guard lock(impl_->mutex);
        auto result = impl_->measured;
        result.failures = impl_->errors->count();
        return result;
    }
    catch (...) { return {}; }
}

void Logger::write_statistics() noexcept
{
    try
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->logger || !impl_->logger->should_log(native_level(LogLevel::Info))) return;
        impl_->flush_repeats();
        impl_->flush_sink();
        const auto &value = impl_->measured;
        const std::string text = "[logger] event=summary write_calls=" + std::to_string(value.write_calls) +
            " emitted_messages=" + std::to_string(value.emitted_messages) +
            " coalesced_messages=" + std::to_string(value.coalesced_messages) +
            " filtered_messages=" + std::to_string(value.filtered_messages) +
            " failures=" + std::to_string(impl_->errors->count()) +
            " lock_wait_ns=" + std::to_string(value.lock_wait_ns) +
            " write_ns=" + std::to_string(value.write_ns) + " max_write_ns=" + std::to_string(value.max_write_ns) +
            " flush_calls=" + std::to_string(value.flush_calls) +
            " flush_lock_wait_ns=" + std::to_string(value.flush_lock_wait_ns) +
            " flush_ns=" + std::to_string(value.flush_ns) + " max_flush_ns=" + std::to_string(value.max_flush_ns) +
            " sink_write_calls=" + std::to_string(value.sink_write_calls) +
            " sink_write_ns=" + std::to_string(value.sink_write_ns) +
            " sink_flush_calls=" + std::to_string(value.sink_flush_calls) +
            " sink_flush_ns=" + std::to_string(value.sink_flush_ns) +
            " write_sink_flush_ns=" + std::to_string(value.write_sink_flush_ns) +
            " write_scope=write_calls_including_lock_wait_and_sink_flush" +
            " flush_scope=explicit_flush_calls_including_lock_wait" +
            " sink_scope=all_submissions_and_explicit_sink_flushes" +
            " sink_write_excludes_sink_flush=1 counters_overlap=1" +
            " snapshot=before_summary_emit caller_formatting=excluded native_logging=separate later_shutdown=excluded";
        impl_->emit(LogLevel::Info, text);
        impl_->previous.clear();
        impl_->previous_module.clear();
        impl_->previous_message.clear();
        impl_->previous_level = LogLevel::Off;
    }
    catch (const std::exception &failure) { impl_->errors->set(failure.what()); }
    catch (...) { impl_->errors->set("unexpected logger summary failure"); }
}

void Logger::flush() noexcept
{
    const auto started = Impl::Clock::now();
    try
    {
        std::lock_guard lock(impl_->mutex);
        const auto acquired = Impl::Clock::now();
        ++impl_->measured.flush_calls;
        impl_->measured.flush_lock_wait_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(acquired - started).count());
        struct FlushTime
        {
            LoggerStatistics &measured;
            Impl::Clock::time_point started;
            ~FlushTime()
            {
                const auto elapsed = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Impl::Clock::now() - started).count());
                measured.flush_ns += elapsed;
                measured.max_flush_ns = std::max(measured.max_flush_ns, elapsed);
            }
        } elapsed{impl_->measured, started};
        if (impl_->logger) { impl_->flush_repeats(); impl_->flush_sink(); }
    }
    catch (const std::exception &failure) { impl_->errors->set(failure.what()); }
    catch (...) { impl_->errors->set("unexpected log flush failure"); }
}

void Logger::shutdown() noexcept
{
    try
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->logger) { impl_->flush_repeats(); impl_->flush_sink(); }
        impl_->logger.reset();
        impl_->previous.clear();
        impl_->previous_module.clear();
        impl_->previous_message.clear();
        impl_->previous_level = LogLevel::Off;
    }
    catch (const std::exception &failure) { impl_->errors->set(failure.what()); }
    catch (...) { impl_->errors->set("unexpected logger shutdown failure"); }
}

bool Logger::ready() const noexcept
{
    try
    {
        std::lock_guard lock(impl_->mutex);
        return static_cast<bool>(impl_->logger);
    }
    catch (...) { return false; }
}

LoggerOptions Logger::options() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->options;
}

std::string Logger::last_error() const { return impl_->errors->get(); }
}
