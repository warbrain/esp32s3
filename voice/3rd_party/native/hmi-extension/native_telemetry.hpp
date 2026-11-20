#pragma once

#include "engine/framework/debug/trace.h"

#include <memory>

// HMI LOGGING / TELEMETRY: optional local file sinks and timing aggregation.
namespace engine::debug {

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

}  // namespace engine::debug
