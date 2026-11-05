#include "engine/framework/debug/trace.h"
#include "environment.hpp"

#include <ggml.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The real diagnostic implementation needs only these two ggml queries.
// No model, backend, microphone, or speaker is involved in this regression.
extern "C" size_t ggml_used_mem(const ggml_context *) { return 0; }
extern "C" size_t ggml_get_mem_size(const ggml_context *) { return 0; }

namespace
{
std::string read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t occurrences(const std::string &text, const std::string &needle)
{
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) ++count;
    return count;
}
}

int main()
{
    if (!hmi_test::initialize_environment()) return 2;
    namespace debug = engine::debug;
    int failures = 0;
    const auto check = [&](bool value, const char *description) {
        std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
        if (!value) ++failures;
    };
    const auto directory = std::filesystem::temp_directory_path() /
        (L"hmi-native-diagnostics-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto output = directory / "native.log";
    const auto legacy = directory / "legacy.log";
    debug::reset_logging();
    check(!debug::log_enabled() && !debug::trace_log_enabled(), "logging starts disabled");
    {
        auto first = std::make_unique<debug::DiagnosticsLease>(output.u8string());
        auto second = std::make_unique<debug::DiagnosticsLease>((directory / "." / "native.log").u8string());
        check(debug::log_enabled() && !debug::trace_log_enabled(), "a lease enables timing without tensor tracing");
        {
            debug::DiagnosticOperation operation("shared", "test", "one", {}, {}, "final");
            debug::timing_log_scalar("compute_ms", 2.0);
            debug::timing_log_scalar("compute_ms", 3.0);
        }
        auto text = read_file(output);
        check(occurrences(text, "event=native_operation") == 1 &&
            text.find("metric.compute_ms={count:2,sum:5,min:2,max:3}") != std::string::npos,
            "same-path leases emit one aggregate with exact count and extrema");
        first.reset();
        {
            debug::DiagnosticOperation operation("surviving", "test", "two");
            debug::timing_log_scalar("compute_ms", 1.0);
            debug::timing_log_scalar("zero_ms", 0.0);
            debug::timing_log_scalar("exact_integer", UINT64_C(18446744073709551615));
            debug::timing_log_scalar("precise_ms", 1.2345678901234567);
            debug::timing_log_scalar("backend", std::string_view("CUDA0"));
        }
        text = read_file(output);
        check(occurrences(text, "event=native_operation") == 2 &&
            occurrences(text, "event=native_logging_summary") == 0,
            "closing one lease leaves the second active without duplicates");
        check(text.find("metric.compute_ms={value:1}") != std::string::npos &&
            text.find("metric.zero_ms={value:0}") != std::string::npos &&
            text.find("metric.exact_integer={value:18446744073709551615}") != std::string::npos &&
            text.find("metric.precise_ms={value:1.2345678901234567}") != std::string::npos &&
            text.find("metric.backend={count:1,last:\"CUDA0\"}") != std::string::npos,
            "single scalars retain exact zero/integer/decimal values without duplicate extrema");
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker)
            workers.emplace_back([worker] {
                for (int item = 0; item < 25; ++item)
                {
                    const auto id = "worker-" + std::to_string(worker) + "-" + std::to_string(item);
                    debug::DiagnosticOperation operation("parallel", "test", id, {}, {}, "parallel");
                    debug::timing_log_scalar("compute_ms", 1.0);
                    debug::timing_log_scalar("compute_ms", 4.0);
                }
            });
        for (auto &worker : workers) worker.join();
        text = read_file(output);
        bool isolated = occurrences(text, "operation=\"parallel\"") == 100 &&
            occurrences(text, "metric.compute_ms={count:2,sum:5,min:1,max:4}") == 100;
        for (int worker = 0; worker < 4; ++worker)
            for (int item = 0; item < 25; ++item)
                isolated = isolated && occurrences(text, "request_id=\"worker-" + std::to_string(worker) +
                    "-" + std::to_string(item) + "\"") == 1;
        check(isolated, "concurrent operations retain isolated request IDs and metrics");
        try {
            debug::DiagnosticOperation operation("throwing", "test", "failed");
            debug::timing_log_scalar("compute_ms", 2.0);
            throw std::runtime_error("expected test failure");
        } catch (const std::runtime_error &) { }
        {
            debug::DiagnosticOperation outer("outer", "test", "outer");
            debug::timing_log_scalar("outer_ms", 1.0);
            {
                debug::DiagnosticOperation inner("inner", "test", "inner");
                debug::timing_log_scalar("inner_ms", 3.0);
            }
            debug::timing_log_scalar("outer_ms", 2.0);
        }
        text = read_file(output);
        const auto failed = text.find("operation=\"throwing\"");
        check(failed != std::string::npos &&
            text.substr(failed, text.find('\n', failed) - failed).find("success=0") != std::string::npos &&
            text.find("metric.outer_ms={count:2,sum:3,min:1,max:2}") != std::string::npos &&
            text.find("metric.inner_ms={value:3}") != std::string::npos,
            "exception and nested scopes unregister safely and restore their parent");
        std::uint64_t lines = 0, write_failures = 0;
        second->statistics(lines, write_failures);
        check(lines == 105 && write_failures == 0, "shared sink counters match complete serialized records");
        const auto measured = second->statistics();
        check(measured.lines == lines && measured.failures == 0 && measured.write_calls == lines &&
            measured.flush_calls == lines && measured.write_ns >= measured.flush_ns &&
            measured.write_ns >= measured.max_write_ns && measured.flush_ns > 0,
            "native I/O statistics include every per-line flush as a subset of writing");
        check(measured.completed_operations == 105 && measured.successful_operations == 104 &&
            measured.failed_operations == 1 && measured.suppressed_operations == 0 &&
            measured.top_level_operations == 104 && measured.operation_wall_ms >= measured.top_level_wall_ms,
            "completed scope snapshots distinguish SDK failures and nested inclusive timings from sink I/O");
    }
    auto summary_text = read_file(output);
    check(occurrences(summary_text, "event=native_logging_summary") == 1 &&
        summary_text.find("lines=105 write_failures=0 write_calls=105") != std::string::npos &&
        summary_text.find("write_includes_flush=1 collection=excluded serialization=excluded lock_wait=excluded") != std::string::npos,
        "the last same-path lease emits exactly one explicitly scoped I/O summary");
    check(!debug::log_enabled(), "last lease restores the initial disabled configuration");
    {
        const auto aggregate_output = directory / "aggregate.log";
        debug::DiagnosticsLease first(aggregate_output.u8string());
        debug::DiagnosticsLease second(aggregate_output.u8string());
        {
            debug::DiagnosticOperation load("model_load", "test");
            check(first.statistics().completed_operations == 0, "in-flight operations are excluded from completed snapshots");
            load.finish();
            load.finish(false);
        }
        {
            debug::DiagnosticOperation create("session_create", "test");
            create.finish(false);
        }
        {
            debug::DiagnosticOperation run("session_run", "test");
            debug::DiagnosticOperation preparation("prepare", "test");
        }
        for (const auto *name : {"stream_start", "stream_push", "stream_finish"})
        {
            debug::DiagnosticOperation stream(name, "test");
        }
        const auto snapshot = first.statistics();
        const auto shared = second.statistics();
        const auto text = read_file(aggregate_output);
        check(snapshot.completed_operations == 7 && snapshot.successful_operations == 6 &&
            snapshot.failed_operations == 1 && snapshot.top_level_operations == 6 &&
            snapshot.lines + snapshot.suppressed_operations == snapshot.completed_operations &&
            snapshot.lines == occurrences(text, "event=native_operation"),
            "fast-operation suppression omits only log lines and finish remains idempotent");
        check(snapshot.completed_operations == shared.completed_operations &&
            snapshot.operation_wall_ms == shared.operation_wall_ms && snapshot.lines == shared.lines &&
            snapshot.model_load_wall_ms > 0.0 && snapshot.session_create_wall_ms > 0.0 &&
            snapshot.session_run_wall_ms > 0.0 && snapshot.stream_wall_ms > 0.0 &&
            snapshot.operation_wall_ms >= snapshot.top_level_wall_ms,
            "same-path snapshot queries share exact category timing without resetting counters");
    }
    {
        auto lease = std::make_unique<debug::DiagnosticsLease>(output.u8string());
        auto operation = std::make_unique<debug::DiagnosticOperation>("in_flight", "test", "retained");
        lease.reset();
        auto replacement = std::make_unique<debug::DiagnosticsLease>(output.u8string());
        debug::timing_log_scalar("compute_ms", 6.0);
        operation.reset();
        std::uint64_t lines = 0, write_failures = 0;
        replacement->statistics(lines, write_failures);
        check(lines == 1 && write_failures == 0,
            "reopening during an in-flight scope reuses its sink and counters");
        check(replacement->statistics().completed_operations == 1,
            "reopened lease retains completed-operation counters from its in-flight scope");
    }
    {
        const auto deferred = directory / "deferred.log";
        auto lease = std::make_unique<debug::DiagnosticsLease>(deferred.u8string());
        auto operation = std::make_unique<debug::DiagnosticOperation>("late_finish", "test", "deferred");
        debug::timing_log_scalar("compute_ms", 8.0);
        lease.reset();
        check(read_file(deferred).empty(), "last lease defers its summary until an in-flight operation ends");
        operation->finish();
        auto text = read_file(deferred);
        check(occurrences(text, "event=native_operation") == 1 &&
            occurrences(text, "event=native_logging_summary") == 1 &&
            text.find("lines=1 write_failures=0 write_calls=1") != std::string::npos,
            "deferred summary includes the final operation without recursion or duplicate output");
        {
            debug::DiagnosticsLease reopened(deferred.u8string());
            debug::DiagnosticOperation fresh("reopened", "test", "new-lifetime");
            debug::timing_log_scalar("compute_ms", 9.0);
        }
        operation.reset();
        text = read_file(deferred);
        check(occurrences(text, "event=native_operation") == 2 &&
            occurrences(text, "event=native_logging_summary") == 2,
            "a completed scope retained by its caller cannot suppress a new same-path sink lifetime");
    }
    debug::configure_logging({true, legacy.u8string()});
    {
        debug::DiagnosticsLease lease(output.u8string());
        check(debug::trace_log_enabled(), "lease preserves an explicitly enabled legacy trace configuration");
    }
    check(debug::log_enabled() && debug::trace_log_enabled(), "releasing a lease preserves legacy logging");
    debug::reset_logging();
    {
        debug::DiagnosticsLease lease(output.u8string());
        debug::configure_logging({true, legacy.u8string()});
        debug::reset_logging();
        check(debug::log_enabled() && !debug::trace_log_enabled(), "legacy reset leaves leased timing active");
        debug::configure_logging({true, legacy.u8string()});
        bool rejected = false;
        try { debug::configure_logging({true, directory.u8string()}); }
        catch (const std::runtime_error &) { rejected = true; }
        debug::log_message(debug::LogLevel::Info, "test", "legacy_after_rejected_configuration");
        check(rejected && read_file(legacy).find("legacy_after_rejected_configuration") != std::string::npos,
            "failed reconfiguration preserves the prior legacy file and enabled state");
        {
            debug::DiagnosticOperation operation("after_reconfigure_failure", "test");
            debug::timing_log_scalar("compute_ms", 7.0);
        }
        check(rejected && read_file(output).find("operation=\"after_reconfigure_failure\"") != std::string::npos,
            "failed legacy file reconfiguration cannot disable an active lease");
        debug::reset_logging();
    }
    check(!debug::log_enabled() && !debug::trace_log_enabled(), "all configuration and lease state is restored");
    std::error_code code;
    std::filesystem::remove_all(directory, code);
    check(!code, "build-local test outputs are closed and removable");
    return failures == 0 ? 0 : 1;
}
