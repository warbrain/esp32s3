#pragma once

#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

struct ggml_context;

namespace engine::debug {

struct LoggingConfig {
    bool enabled = false;
    std::optional<std::string> file_path = std::nullopt;
};

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

std::string_view to_string(LogLevel level) noexcept;

void configure_logging(const LoggingConfig & config);
void reset_logging();
bool log_enabled();
void log_message(std::string_view line);
void log_message(LogLevel level, std::string_view category, std::string_view message);
bool trace_log_enabled();
bool timing_log_enabled();
std::string format_dims(const std::vector<int64_t> & dims);
std::vector<size_t> sample_point_indices(size_t count, size_t target = 40);
std::string sample_points_f32(const std::vector<float> & values);
std::string sample_points_i32(const std::vector<int32_t> & values);
void trace_log_f32(const std::string & name, const std::vector<int64_t> & dims, const std::vector<float> & values);
void trace_log_i32(const std::string & name, const std::vector<int64_t> & dims, const std::vector<int32_t> & values);
void trace_log_scalar(const std::string & name, std::string_view value);
void trace_log_scalar(const std::string & name, double value);
void trace_log_scalar(const std::string & name, int64_t value);
void trace_log_scalar(const std::string & name, uint64_t value);
void trace_log_scalar(const std::string & name, bool value);
void timing_log_scalar(const std::string & name, std::string_view value);
void timing_log_scalar(const std::string & name, double value);
void timing_log_scalar(const std::string & name, int64_t value);
void timing_log_scalar(const std::string & name, uint64_t value);
void timing_log_scalar(const std::string & name, bool value);

struct DiagnosticTarget;
struct DiagnosticOperationState;

struct DiagnosticStatistics {
    std::uint64_t lines = 0;
    std::uint64_t failures = 0;
    std::uint64_t write_calls = 0;
    std::uint64_t write_ns = 0; // Sink I/O including reopen/rotation and flush; excludes collection/formatting/lock wait.
    std::uint64_t max_write_ns = 0;
    std::uint64_t flush_calls = 0;
    std::uint64_t flush_ns = 0; // Included in write_ns.
    std::uint64_t completed_operations = 0;
    std::uint64_t successful_operations = 0;
    std::uint64_t failed_operations = 0;
    std::uint64_t suppressed_operations = 0;
    std::uint64_t top_level_operations = 0;
    double operation_wall_ms = 0.0; // Inclusive sum; nested and concurrent scopes overlap.
    double top_level_wall_ms = 0.0; // Nested scopes excluded; concurrent threads still overlap.
    double model_load_wall_ms = 0.0;
    double session_create_wall_ms = 0.0;
    double session_run_wall_ms = 0.0;
    double stream_wall_ms = 0.0; // stream_start, stream_push and stream_finish.
};

// Process diagnostics use leases so one SDK client cannot disable another.
// Reusing the same path shares one rotating output instead of duplicating lines.
// One sink-I/O summary is emitted when its last lease and in-flight scope end.
class DiagnosticsLease {
public:
    explicit DiagnosticsLease(const std::string & path);
    ~DiagnosticsLease();
    DiagnosticsLease(const DiagnosticsLease &) = delete;
    DiagnosticsLease & operator=(const DiagnosticsLease &) = delete;
    void statistics(std::uint64_t & lines, std::uint64_t & failures) const noexcept;
    DiagnosticStatistics statistics() const noexcept;
private:
    std::shared_ptr<DiagnosticTarget> target_;
};

// Aggregates existing timing scalars within a C ABI operation. The scope owns
// all metrics; registration uses the existing logger's mutex and state.
class DiagnosticOperation {
public:
    DiagnosticOperation(std::string_view operation, std::string_view family = {},
                        std::string_view request_id = {}, std::string_view segment_index = {},
                        std::string_view attempt = {}, std::string_view stage = {},
                        std::string_view revision = {});
    ~DiagnosticOperation();
    DiagnosticOperation(const DiagnosticOperation &) = delete;
    DiagnosticOperation & operator=(const DiagnosticOperation &) = delete;
    void finish(bool success = true) noexcept;
private:
    std::unique_ptr<DiagnosticOperationState> state_;
    std::uint64_t range_id_ = 0;
    int exceptions_ = 0;
    bool finished_ = false;
};

bool nvtx_compiled() noexcept;
// Logs `<name>.ctx_reserved_mb` (the ggml_init mem_size, a host allocation
// made in full up front) and `<name>.ctx_used_mb` (what the tensor headers in
// it actually take). For a no_alloc context the gap is memory the process
// charges but never touches; call it once the graph is built.
void timing_log_context_reservation(const std::string & name, const ggml_context * ctx);

template <typename Integer, std::enable_if_t<std::is_integral_v<Integer> && !std::is_same_v<std::decay_t<Integer>, bool>, int> = 0>
inline void trace_log_scalar(const std::string & name, Integer value) {
    if constexpr (std::is_signed_v<Integer>) {
        trace_log_scalar(name, static_cast<int64_t>(value));
    } else {
        trace_log_scalar(name, static_cast<uint64_t>(value));
    }
}

template <typename Integer, std::enable_if_t<std::is_integral_v<Integer> && !std::is_same_v<std::decay_t<Integer>, bool>, int> = 0>
inline void timing_log_scalar(const std::string & name, Integer value) {
    if constexpr (std::is_signed_v<Integer>) {
        timing_log_scalar(name, static_cast<int64_t>(value));
    } else {
        timing_log_scalar(name, static_cast<uint64_t>(value));
    }
}

}  // namespace engine::debug
