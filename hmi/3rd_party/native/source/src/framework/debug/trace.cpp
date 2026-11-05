#include "engine/framework/debug/trace.h"

#include <ggml.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <map>
#include <thread>
#include <exception>
#include <cmath>

#if defined(ENGINE_HAS_NVTX)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <nvtx3/nvToolsExt.h>
#endif

namespace engine::debug {

struct DiagnosticTarget {
    using Clock = std::chrono::steady_clock;
    std::filesystem::path path;
    std::ofstream file;
    std::uintmax_t bytes = 0;
    std::size_t owners = 0;
    std::size_t operations = 0;
    bool completed = false;
    DiagnosticStatistics measured;

    struct Measure {
        std::uint64_t & total;
        std::uint64_t * maximum = nullptr;
        Clock::time_point started = Clock::now();
        ~Measure() {
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
            total += elapsed;
            if (maximum) *maximum = std::max(*maximum, elapsed);
        }
    };

    explicit DiagnosticTarget(const std::string & name) : path(std::filesystem::u8path(name)) {
        if (path.empty()) throw std::invalid_argument("diagnostic file path must be non-empty");
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::error_code error;
        bytes = std::filesystem::file_size(path, error);
        if (error) bytes = 0;
        file.open(path, std::ios::binary | std::ios::app);
        if (!file) throw std::runtime_error("cannot open native diagnostic file");
    }

    void finish_if_unused() noexcept {
        if (completed || owners != 0 || operations != 0) return;
        completed = true;
        // Caller holds the logger mutex, also serializing a new same-path lease.
        try {
            if (measured.write_calls == 0) { file.close(); return; }
            std::ostringstream summary;
            summary << "event=native_logging_summary scope=shared_file_sink_lifetime lines=" << measured.lines
                    << " write_failures=" << measured.failures << " write_calls=" << measured.write_calls
                    << " write_ns=" << measured.write_ns << " max_write_ns=" << measured.max_write_ns
                    << " flush_calls=" << measured.flush_calls << " flush_ns=" << measured.flush_ns
                    << " write_includes_flush=1 collection=excluded serialization=excluded lock_wait=excluded"
                    << " legacy_output=excluded snapshot=before_summary_emit";
            write_noexcept(summary.str());
            file.close();
        } catch (...) { }
    }

    void write(const std::string & line) {
        if (!file.is_open()) {
            file.clear();
            file.open(path, std::ios::binary | std::ios::app);
            std::error_code error;
            bytes = std::filesystem::file_size(path, error);
            if (error) bytes = 0;
            if (!file) throw std::runtime_error("cannot reopen native diagnostic file");
        }
        constexpr std::uintmax_t maximum = 5 * 1024 * 1024;
        if (bytes != 0 && bytes + line.size() + 1 > maximum) {
            file.close();
            std::error_code error;
            auto backup = path;
            backup += ".3";
            std::filesystem::remove(backup, error);
            for (int index = 2; index >= 1; --index) {
                auto from = path;
                from += "." + std::to_string(index);
                auto to = path;
                to += "." + std::to_string(index + 1);
                error.clear();
                if (std::filesystem::exists(from, error)) std::filesystem::rename(from, to, error);
                if (error) throw std::runtime_error("cannot rotate native diagnostic backup");
            }
            backup = path;
            backup += ".1";
            std::filesystem::rename(path, backup, error);
            if (error) throw std::runtime_error("cannot rotate native diagnostic file");
            file.open(path, std::ios::binary | std::ios::trunc);
            if (!file) throw std::runtime_error("cannot reopen native diagnostic file");
            bytes = 0;
        }
        file << line << '\n';
        {
            ++measured.flush_calls;
            Measure elapsed{measured.flush_ns};
            file.flush();
        }
        if (!file) throw std::runtime_error("cannot write native diagnostic file");
        bytes += line.size() + 1;
        ++measured.lines;
    }

    void write_noexcept(const std::string & line) noexcept {
        ++measured.write_calls;
        Measure elapsed{measured.write_ns, &measured.max_write_ns};
        try { write(line); } catch (...) { ++measured.failures; file.close(); }
    }
};

struct DiagnosticMetric {
    std::uint64_t count = 0;
    double total = 0;
    double minimum = 0;
    double maximum = 0;
    std::string last;
    bool numeric = true;
};

struct DiagnosticOperationState {
    std::string operation;
    std::string family;
    std::string request_id;
    std::string segment_index;
    std::string attempt;
    std::string stage;
    std::string revision;
    std::thread::id thread = std::this_thread::get_id();
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::map<std::string, DiagnosticMetric> metrics;
    std::vector<std::shared_ptr<DiagnosticTarget>> targets;
    bool top_level = false;
};

namespace {

struct LoggerState {
    bool configured_enabled = false;
    std::atomic_bool trace_enabled{false};
    std::vector<std::weak_ptr<DiagnosticTarget>> targets;
    std::map<std::thread::id, std::vector<DiagnosticOperationState *>> operations;
    std::optional<std::string> file_path;
    std::unique_ptr<std::ofstream> file_stream;
};

LoggerState & logger_state() {
    static LoggerState state;
    return state;
}

std::mutex & logger_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::atomic_bool & logger_enabled_flag() {
    static std::atomic_bool enabled{false};
    return enabled;
}

std::ostream & logger_output_locked(LoggerState & state) {
    if (!state.file_path.has_value() || state.file_path->empty()) {
        return std::cout;
    }
    if (!state.file_stream || !state.file_stream->is_open()) {
        state.file_stream = std::make_unique<std::ofstream>(std::filesystem::u8path(*state.file_path), std::ios::app);
        if (!state.file_stream->is_open()) {
            throw std::runtime_error("Failed to open log file: " + *state.file_path);
        }
    }
    return *state.file_stream;
}

std::string timestamp_seconds_local() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif
    std::ostringstream out;
    out << std::put_time(&local_tm, "%Y%m%d-%H%M%S");
    return out.str();
}

std::string format_scalar_double(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (result.ec == std::errc()) {
        return std::string(buffer, result.ptr);
    }
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

void refresh_enabled_locked(LoggerState & state) {
    state.targets.erase(std::remove_if(state.targets.begin(), state.targets.end(), [](const auto & weak) {
        return weak.expired();
    }), state.targets.end());
    const bool leased = std::any_of(state.targets.begin(), state.targets.end(), [](const auto & weak) {
        const auto target = weak.lock();
        return target && target->owners != 0;
    });
    logger_enabled_flag().store(state.configured_enabled || leased || !state.operations.empty(),
                                std::memory_order_release);
}

std::string quoted(std::string_view value) {
    std::string result = "\"";
    for (const unsigned char byte : value) {
        if (byte == '\\' || byte == '"') { result += '\\'; result += static_cast<char>(byte); }
        else if (byte == '\n') result += "\\n";
        else if (byte == '\r') result += "\\r";
        else if (byte == '\t') result += "\\t";
        else if (byte < 0x20) result += '?';
        else result += static_cast<char>(byte);
    }
    result += '"';
    return result;
}

bool collect_timing(std::string_view name, std::string_view value) noexcept {
    try {
        std::lock_guard<std::mutex> lock(logger_mutex());
        auto & state = logger_state();
        const auto found = state.operations.find(std::this_thread::get_id());
        if (found == state.operations.end() || found->second.empty()) return false;
        auto & metrics = found->second.back()->metrics;
        if (name.size() > 256 || value.size() > 1024 || (metrics.size() >= 512 && metrics.find(std::string(name)) == metrics.end()))
            return true;
        auto & metric = metrics[std::string(name)];
        double numeric = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), numeric);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(numeric))
            metric.numeric = false;
        if (metric.count == 0) metric.minimum = metric.maximum = numeric;
        else { metric.minimum = std::min(metric.minimum, numeric); metric.maximum = std::max(metric.maximum, numeric); }
        metric.total += numeric;
        ++metric.count;
        metric.last = value;
    } catch (...) { }
    return true;
}
}  // namespace

DiagnosticsLease::DiagnosticsLease(const std::string & path) {
    std::lock_guard<std::mutex> lock(logger_mutex());
    auto & state = logger_state();
    const auto absolute = std::filesystem::absolute(std::filesystem::u8path(path)).lexically_normal();
    for (const auto & weak : state.targets) {
        const auto existing = weak.lock();
        if (existing && !existing->completed) {
            std::error_code error;
            if (existing->path == absolute || std::filesystem::equivalent(existing->path, absolute, error)) {
                target_ = existing;
                break;
            }
        }
    }
    if (!target_) {
        const auto utf8 = absolute.u8string();
        target_ = std::make_shared<DiagnosticTarget>(std::string(utf8.begin(), utf8.end()));
        state.targets.push_back(target_);
    }
    ++target_->owners;
    refresh_enabled_locked(state);
}

DiagnosticsLease::~DiagnosticsLease() {
    try {
        std::lock_guard<std::mutex> lock(logger_mutex());
        if (target_ && target_->owners != 0) {
            --target_->owners;
            target_->finish_if_unused();
        }
        refresh_enabled_locked(logger_state());
    } catch (...) { }
}

void DiagnosticsLease::statistics(std::uint64_t & lines, std::uint64_t & failures) const noexcept {
    const auto value = statistics();
    lines = value.lines;
    failures = value.failures;
}

DiagnosticStatistics DiagnosticsLease::statistics() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(logger_mutex());
        if (target_) return target_->measured;
    } catch (...) { }
    return {};
}

bool nvtx_compiled() noexcept {
#if defined(ENGINE_HAS_NVTX)
    return true;
#else
    return false;
#endif
}

DiagnosticOperation::DiagnosticOperation(std::string_view operation, std::string_view family,
                                       std::string_view request_id, std::string_view segment_index,
                                       std::string_view attempt, std::string_view stage, std::string_view revision)
    : exceptions_(std::uncaught_exceptions()) {
    try {
        const auto effective_stage = stage.empty() ? operation : stage;
#if defined(ENGINE_HAS_NVTX)
        const std::string label = "native." + std::string(family) + "." + std::string(effective_stage) + "." +
            std::string(operation) + " request_id=" + std::string(request_id) + " segment=" + std::string(segment_index);
        range_id_ = static_cast<std::uint64_t>(nvtxRangeStartA(label.c_str()));
#endif
        if (!log_enabled()) return;
        std::lock_guard<std::mutex> lock(logger_mutex());
        auto & logger = logger_state();
        auto state = std::make_unique<DiagnosticOperationState>();
        for (const auto & weak : logger.targets) {
            const auto target = weak.lock();
            if (target && target->owners != 0) state->targets.push_back(target);
        }
        if (state->targets.empty()) return;
        state->operation = operation;
        state->family = family;
        state->request_id = request_id;
        state->segment_index = segment_index;
        state->attempt = attempt;
        state->stage = effective_stage;
        state->revision = revision;
        auto & scopes = logger.operations[state->thread];
        state->top_level = scopes.empty();
        try { scopes.push_back(state.get()); }
        catch (...) {
            if (scopes.empty()) logger.operations.erase(state->thread);
            throw;
        }
        state_ = std::move(state);
        for (const auto & target : state_->targets) ++target->operations;
    } catch (...) { }
}

DiagnosticOperation::~DiagnosticOperation() {
    finish(std::uncaught_exceptions() == exceptions_);
}

void DiagnosticOperation::finish(bool success) noexcept {
    if (finished_) return;
    finished_ = true;
#if defined(ENGINE_HAS_NVTX)
    if (range_id_ != 0) nvtxRangeEnd(static_cast<nvtxRangeId_t>(range_id_));
#endif
    if (!state_) return;
    try {
        std::lock_guard<std::mutex> lock(logger_mutex());
        struct CompletedOperation {
            const std::vector<std::shared_ptr<DiagnosticTarget>> & targets;
            ~CompletedOperation() {
                for (const auto & target : targets) {
                    if (target->operations != 0) --target->operations;
                    target->finish_if_unused();
                }
            }
        } completed{state_->targets};
        auto & logger = logger_state();
        const auto found = logger.operations.find(state_->thread);
        if (found != logger.operations.end()) {
            auto & scopes = found->second;
            scopes.erase(std::remove(scopes.begin(), scopes.end(), state_.get()), scopes.end());
            if (scopes.empty()) logger.operations.erase(found);
        }
        refresh_enabled_locked(logger);
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - state_->started).count();
        const bool suppressed = success && (state_->operation == "frontend_push" ||
            (state_->metrics.empty() && elapsed < 1.0 &&
             (state_->operation == "stream_push" || state_->operation == "prepare")));
        for (const auto & target : state_->targets) {
            auto & measured = target->measured;
            ++measured.completed_operations;
            if (success) ++measured.successful_operations;
            else ++measured.failed_operations;
            if (suppressed) ++measured.suppressed_operations;
            measured.operation_wall_ms += elapsed;
            if (state_->top_level) {
                ++measured.top_level_operations;
                measured.top_level_wall_ms += elapsed;
            }
            if (state_->operation == "model_load") measured.model_load_wall_ms += elapsed;
            else if (state_->operation == "session_create") measured.session_create_wall_ms += elapsed;
            else if (state_->operation == "session_run") measured.session_run_wall_ms += elapsed;
            else if (state_->operation == "stream_start" || state_->operation == "stream_push" ||
                     state_->operation == "stream_finish") measured.stream_wall_ms += elapsed;
        }
        if (suppressed) return;
        std::ostringstream line;
        const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        line << "event=native_operation unix_ms=" << wall_ms << " thread=" << state_->thread
             << " operation=" << quoted(state_->operation) << " family=" << quoted(state_->family)
             << " request_id=" << quoted(state_->request_id) << " segment_index=" << quoted(state_->segment_index)
             << " attempt=" << quoted(state_->attempt) << " success=" << (success ? 1 : 0)
             << " stage=" << quoted(state_->stage) << " revision=" << quoted(state_->revision)
             << " elapsed_ms=" << elapsed << " nvtx=" << (nvtx_compiled() ? "compiled" : "unavailable");
        for (const auto & entry : state_->metrics) {
            const auto & metric = entry.second;
            line << " metric." << entry.first << "={";
            if (metric.numeric && metric.count == 1) line << "value:" << metric.last;
            else {
                line << "count:" << metric.count;
                if (metric.numeric) {
                    line << ",sum:" << metric.total << ",min:" << metric.minimum << ",max:" << metric.maximum;
                } else line << ",last:" << quoted(metric.last);
            }
            line << '}';
        }
        const auto output = line.str();
        for (const auto & target : state_->targets) {
            target->write_noexcept(output);
        }
    } catch (...) { }
}
std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    }
    return "unknown";
}

void configure_logging(const LoggingConfig & config) {
    std::lock_guard<std::mutex> lock(logger_mutex());
    auto & state = logger_state();
    auto next_path = config.file_path;
    const bool path_changed = state.file_path != next_path;
    std::unique_ptr<std::ofstream> prepared;
    if (config.enabled && next_path.has_value() && !next_path->empty() &&
        (path_changed || !state.file_stream || !state.file_stream->is_open())) {
        prepared = std::make_unique<std::ofstream>(std::filesystem::u8path(*next_path), std::ios::app);
        if (!prepared->is_open()) throw std::runtime_error("Failed to open log file: " + *next_path);
    }
    if (path_changed || prepared) state.file_stream = std::move(prepared);
    state.file_path = std::move(next_path);
    state.configured_enabled = config.enabled;
    state.trace_enabled.store(config.enabled, std::memory_order_release);
    refresh_enabled_locked(state);
}

void reset_logging() {
    configure_logging(LoggingConfig{});
}

bool log_enabled() {
    return logger_enabled_flag().load(std::memory_order_acquire);
}

void log_message(std::string_view line) {
    if (!log_enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(logger_mutex());
    if (!log_enabled()) {
        return;
    }
    auto & state = logger_state();
    if (state.configured_enabled) {
        auto & output = logger_output_locked(state);
        output << line << "\n";
        output.flush();
    }
    if (line.find("[TIMING") == 0 || line.find("[warning]") == 0 || line.find("[error]") == 0) {
        std::ostringstream prefix;
        prefix << "event=native_message unix_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() << " thread=" << std::this_thread::get_id();
        const auto found = state.operations.find(std::this_thread::get_id());
        if (found != state.operations.end() && !found->second.empty()) {
            const auto & operation = *found->second.back();
            prefix << " operation=" << quoted(operation.operation) << " family=" << quoted(operation.family)
                   << " request_id=" << quoted(operation.request_id) << " stage=" << quoted(operation.stage)
                   << " revision=" << quoted(operation.revision) << " segment_index=" << quoted(operation.segment_index)
                   << " attempt=" << quoted(operation.attempt);
        } else prefix << " operation=unscoped stage=unscoped";
        prefix << " detail=" << quoted(line);
        for (const auto & weak : state.targets) {
            const auto target = weak.lock();
            if (target && target->owners != 0) {
                target->write_noexcept(prefix.str());
            }
        }
    }
}

void log_message(LogLevel level, std::string_view category, std::string_view message) {
    if (!log_enabled()) {
        return;
    }
    std::ostringstream out;
    out << "[" << to_string(level) << "]";
    if (!category.empty()) {
        out << "[" << category << "]";
    }
    out << " " << message;
    log_message(out.str());
}

bool trace_log_enabled() {
    return logger_state().trace_enabled.load(std::memory_order_acquire);
}

bool timing_log_enabled() {
    return log_enabled();
}

std::string format_dims(const std::vector<int64_t> & dims) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < dims.size(); ++i) {
        if (i != 0) {
            oss << ",";
        }
        oss << dims[i];
    }
    oss << "]";
    return oss.str();
}

std::vector<size_t> sample_point_indices(size_t count, size_t target) {
    if (count == 0) {
        return {};
    }
    if (count <= target) {
        std::vector<size_t> all(count);
        for (size_t i = 0; i < count; ++i) {
            all[i] = i;
        }
        return all;
    }

    const size_t first_count = 14;
    const size_t middle_count = 12;
    const size_t last_count = 14;
    const size_t first_end = count / 3;
    const size_t middle_end = (count * 2) / 3;

    auto append_range = [](std::vector<size_t> & dst, size_t begin, size_t end, size_t samples) {
        if (begin >= end || samples == 0) {
            return;
        }
        const size_t span = end - begin;
        if (span <= samples) {
            for (size_t i = begin; i < end; ++i) {
                if (dst.empty() || dst.back() != i) {
                    dst.push_back(i);
                }
            }
            return;
        }
        for (size_t i = 0; i < samples; ++i) {
            const double pos = samples == 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(samples - 1);
            const size_t offset = static_cast<size_t>(pos * static_cast<double>(span - 1));
            const size_t index = begin + offset;
            if (dst.empty() || dst.back() != index) {
                dst.push_back(index);
            }
        }
    };

    std::vector<size_t> points;
    points.reserve(target);
    append_range(points, 0, first_end, first_count);
    append_range(points, first_end, middle_end, middle_count);
    append_range(points, middle_end, count, last_count);

    if (points.empty() || points.front() != 0) {
        points.insert(points.begin(), 0);
    }
    if (points.back() != count - 1) {
        points.push_back(count - 1);
    }
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

std::string sample_points_f32(const std::vector<float> & values) {
    if (values.empty()) {
        return "[]";
    }
    const std::vector<size_t> points = sample_point_indices(values.size());
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < points.size(); ++i) {
        if (i != 0) {
            oss << ",";
        }
        const size_t index = points[i];
        oss << index << ":" << values[index];
    }
    oss << "]";
    return oss.str();
}

std::string sample_points_i32(const std::vector<int32_t> & values) {
    if (values.empty()) {
        return "[]";
    }
    const std::vector<size_t> points = sample_point_indices(values.size());
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < points.size(); ++i) {
        if (i != 0) {
            oss << ",";
        }
        const size_t index = points[i];
        oss << index << ":" << values[index];
    }
    oss << "]";
    return oss.str();
}

void trace_log_f32(const std::string & name, const std::vector<int64_t> & dims, const std::vector<float> & values) {
    if (!trace_log_enabled()) {
        return;
    }
    std::ostringstream output;
    output << "[TRACE ts=" << timestamp_seconds_local() << "] " << name
        << " shape=" << format_dims(dims)
        << " size=" << values.size()
        << " samples=" << sample_points_f32(values);
    log_message(output.str());
}

void trace_log_i32(const std::string & name, const std::vector<int64_t> & dims, const std::vector<int32_t> & values) {
    if (!trace_log_enabled()) {
        return;
    }
    std::ostringstream output;
    output << "[TRACE ts=" << timestamp_seconds_local() << "] " << name
        << " shape=" << format_dims(dims)
        << " size=" << values.size()
        << " samples=" << sample_points_i32(values);
    log_message(output.str());
}

void trace_log_scalar(const std::string & name, std::string_view value) {
    if (!trace_log_enabled()) {
        return;
    }
    std::ostringstream output;
    output << "[TRACE ts=" << timestamp_seconds_local() << "] " << name << " " << value;
    log_message(output.str());
}

void trace_log_scalar(const std::string & name, double value) {
    if (!trace_log_enabled()) {
        return;
    }
    trace_log_scalar(name, format_scalar_double(value));
}

void trace_log_scalar(const std::string & name, int64_t value) {
    if (!trace_log_enabled()) {
        return;
    }
    trace_log_scalar(name, std::to_string(value));
}

void trace_log_scalar(const std::string & name, uint64_t value) {
    if (!trace_log_enabled()) {
        return;
    }
    trace_log_scalar(name, std::to_string(value));
}

void trace_log_scalar(const std::string & name, bool value) {
    if (!trace_log_enabled()) {
        return;
    }
    trace_log_scalar(name, std::string_view(value ? "1" : "0"));
}

void timing_log_scalar(const std::string & name, std::string_view value) {
    if (!log_enabled()) return;
    if (collect_timing(name, value)) return;
    if (!timing_log_enabled()) {
        return;
    }
    std::ostringstream output;
    output << "[TIMING ts=" << timestamp_seconds_local() << "] " << name << " " << value;
    log_message(output.str());
}

void timing_log_scalar(const std::string & name, double value) {
    if (!timing_log_enabled()) {
        return;
    }
    timing_log_scalar(name, format_scalar_double(value));
}

void timing_log_scalar(const std::string & name, int64_t value) {
    if (!timing_log_enabled()) {
        return;
    }
    timing_log_scalar(name, std::to_string(value));
}

void timing_log_scalar(const std::string & name, uint64_t value) {
    if (!timing_log_enabled()) {
        return;
    }
    timing_log_scalar(name, std::to_string(value));
}

void timing_log_context_reservation(const std::string & name, const ggml_context * ctx) {
    if (!timing_log_enabled() || ctx == nullptr) {
        return;
    }
    constexpr double kMiB = 1024.0 * 1024.0;
    timing_log_scalar(name + ".ctx_reserved_mb", static_cast<double>(ggml_get_mem_size(ctx)) / kMiB);
    timing_log_scalar(name + ".ctx_used_mb", static_cast<double>(ggml_used_mem(ctx)) / kMiB);
}

void timing_log_scalar(const std::string & name, bool value) {
    if (!timing_log_enabled()) {
        return;
    }
    timing_log_scalar(name, std::string_view(value ? "1" : "0"));
}

}  // namespace engine::debug
