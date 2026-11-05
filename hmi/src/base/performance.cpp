#include "base/performance.hpp"
#include "base/logger.hpp"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace hmi::base
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr double kMib = 1024.0 * 1024.0;

std::string status_code(const char *operation, unsigned long code)
{
    char message[160]{};
    std::snprintf(message, sizeof(message), "%s (0x%08lx)", operation, code);
    return message;
}

std::string measurement(bool available, double value)
{
    if (!available) return "N/A";
    char text[48]{};
    std::snprintf(text, sizeof(text), "%.2f", value);
    return text;
}

std::uint64_t filetime_ticks(FILETIME value)
{
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

struct CpuReading
{
    bool valid = false;
    std::uint64_t process_ticks = 0;
    Clock::time_point wall;
};

bool cpu_reading(CpuReading &previous, double &percent)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    {
        previous.valid = false;
        return false;
    }
    const auto now = Clock::now();
    const auto ticks = filetime_ticks(kernel) + filetime_ticks(user);
    const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const double elapsed = std::chrono::duration<double>(now - previous.wall).count();
    const bool available = previous.valid && processors != 0 && elapsed > 0.0 && ticks >= previous.process_ticks;
    if (available)
        percent = std::clamp(100.0 * static_cast<double>(ticks - previous.process_ticks) /
            (elapsed * 10000000.0 * static_cast<double>(processors)), 0.0, 100.0);
    previous = {true, ticks, now};
    return available;
}

struct ProcessReading
{
    bool faults_valid = false;
    DWORD previous_faults = 0;
    bool io_valid = false;
    IO_COUNTERS previous_io{};
    Clock::time_point io_wall;

    void sample(PerformanceSnapshot &out)
    {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = static_cast<DWORD>(sizeof(counters));
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                   static_cast<DWORD>(sizeof(counters))))
        {
            out.memory_available = out.private_commit_available = out.page_faults_available = true;
            out.working_set_mib = static_cast<double>(counters.WorkingSetSize) / kMib;
            out.private_commit_mib = static_cast<double>(counters.PrivateUsage) / kMib;
            out.page_faults_total = counters.PageFaultCount;
            out.page_fault_delta_available = faults_valid;
            if (faults_valid) out.page_faults_delta = static_cast<DWORD>(counters.PageFaultCount - previous_faults);
            previous_faults = counters.PageFaultCount;
            faults_valid = true;
        }
        else faults_valid = false;
        IO_COUNTERS io{};
        const auto now = Clock::now();
        if (GetProcessIoCounters(GetCurrentProcess(), &io))
        {
            out.io_available = true;
            out.io_read_bytes = io.ReadTransferCount;
            out.io_write_bytes = io.WriteTransferCount;
            out.io_read_operations = io.ReadOperationCount;
            out.io_write_operations = io.WriteOperationCount;
            const double seconds = std::chrono::duration<double>(now - io_wall).count();
            out.io_rate_available = io_valid && seconds > 0.0 &&
                io.ReadTransferCount >= previous_io.ReadTransferCount && io.WriteTransferCount >= previous_io.WriteTransferCount &&
                io.ReadOperationCount >= previous_io.ReadOperationCount && io.WriteOperationCount >= previous_io.WriteOperationCount;
            if (out.io_rate_available)
            {
                out.io_read_bytes_delta = io.ReadTransferCount - previous_io.ReadTransferCount;
                out.io_write_bytes_delta = io.WriteTransferCount - previous_io.WriteTransferCount;
                out.io_read_operations_delta = io.ReadOperationCount - previous_io.ReadOperationCount;
                out.io_write_operations_delta = io.WriteOperationCount - previous_io.WriteOperationCount;
                out.io_read_mib_per_second = static_cast<double>(out.io_read_bytes_delta) / (seconds * kMib);
                out.io_write_mib_per_second = static_cast<double>(out.io_write_bytes_delta) / (seconds * kMib);
            }
            previous_io = io;
            io_wall = now;
            io_valid = true;
        }
        else io_valid = false;
    }
};

bool process_instance(std::wstring_view name, std::wstring_view prefix)
{
    return name.starts_with(prefix);
}

struct CounterValue
{
    bool available = false;
    double value = 0.0;
    std::string detail;
};

enum class EngineGroup { All, Compute, Graphics, Copy };

bool engine_group(std::wstring_view name, EngineGroup group)
{
    if (group == EngineGroup::All) return true;
    if (group == EngineGroup::Compute)
        return name.find(L"engtype_Compute") != std::wstring_view::npos || name.find(L"engtype_CUDA") != std::wstring_view::npos;
    if (group == EngineGroup::Graphics) return name.find(L"engtype_3D") != std::wstring_view::npos;
    return name.find(L"engtype_Copy") != std::wstring_view::npos;
}

CounterValue aggregate_counter_items(const PDH_FMT_COUNTERVALUE_ITEM_W *items, DWORD count,
                                    std::wstring_view pid_prefix, bool sum, EngineGroup group = EngineGroup::All)
{
    CounterValue result;
    for (DWORD index = 0; index < count; ++index)
    {
        const auto &item = items[index];
        if (!item.szName || !process_instance(item.szName, pid_prefix) || !engine_group(item.szName, group) ||
            (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) ||
            !std::isfinite(item.FmtValue.doubleValue) || item.FmtValue.doubleValue < 0.0)
            continue;
        result.available = true;
        result.value = sum ? result.value + item.FmtValue.doubleValue :
            std::max(result.value, item.FmtValue.doubleValue);
    }
    if (!result.available) result.detail = "no valid current-process instances";
    else if (!std::isfinite(result.value)) return {false, 0.0, "GPU counter result is not finite"};
    return result;
}

CounterValue counter_value(PDH_HCOUNTER counter, std::wstring_view pid_prefix, bool sum,
                           PerformanceSnapshot *engines = nullptr)
{
    if (!counter) return {false, 0.0, "counter unavailable"};
    // The instance set can change while PDH sizes its buffer. Retry a bounded
    // number of times instead of retaining pointers into a resized allocation.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        DWORD bytes = 0;
        DWORD count = 0;
        PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (status == ERROR_SUCCESS && bytes == 0) return {false, 0.0, "no current-process instances"};
        if (status != PDH_MORE_DATA)
            return {false, 0.0, status_code("GPU counter read failed", static_cast<unsigned long>(status))};
        if (bytes == 0 || bytes > 16 * 1024 * 1024)
            return {false, 0.0, "GPU counter array exceeds the sampling limit"};
        std::vector<std::max_align_t> storage((static_cast<std::size_t>(bytes) + sizeof(std::max_align_t) - 1) /
            sizeof(std::max_align_t));
        auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(storage.data());
        status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items);
        if (status == PDH_MORE_DATA) continue;
        if (status != ERROR_SUCCESS)
            return {false, 0.0, status_code("GPU counter read failed", static_cast<unsigned long>(status))};
        if (engines)
        {
            const auto compute = aggregate_counter_items(items, count, pid_prefix, false, EngineGroup::Compute);
            const auto graphics = aggregate_counter_items(items, count, pid_prefix, false, EngineGroup::Graphics);
            const auto copy = aggregate_counter_items(items, count, pid_prefix, false, EngineGroup::Copy);
            engines->gpu_compute_available = compute.available;
            engines->gpu_graphics_available = graphics.available;
            engines->gpu_copy_available = copy.available;
            if (compute.available) engines->gpu_compute_percent = std::clamp(compute.value, 0.0, 100.0);
            if (graphics.available) engines->gpu_graphics_percent = std::clamp(graphics.value, 0.0, 100.0);
            if (copy.available) engines->gpu_copy_percent = std::clamp(copy.value, 0.0, 100.0);
        }
        return aggregate_counter_items(items, count, pid_prefix, sum);
    }
    return {false, 0.0, "GPU instance set changed during sampling"};
}

struct GpuCounters
{
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER engine = nullptr;
    PDH_HCOUNTER memory = nullptr;
    std::wstring pid_prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    std::string engine_error;
    std::string memory_error;

    GpuCounters() = default;
    GpuCounters(const GpuCounters &) = delete;
    GpuCounters &operator=(const GpuCounters &) = delete;
    ~GpuCounters() { if (query) PdhCloseQuery(query); }

    void initialize()
    {
        PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &query);
        if (status != ERROR_SUCCESS)
        {
            engine_error = status_code("GPU PDH query unavailable", static_cast<unsigned long>(status));
            return;
        }
        status = PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engine);
        if (status != ERROR_SUCCESS)
        {
            engine = nullptr;
            engine_error = status_code("GPU engine counter unavailable", static_cast<unsigned long>(status));
        }
        status = PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &memory);
        if (status != ERROR_SUCCESS)
        {
            memory = nullptr;
            memory_error = status_code("GPU memory counter unavailable", static_cast<unsigned long>(status));
        }
        if (engine || memory) (void)PdhCollectQueryData(query);
    }

    void sample(PerformanceSnapshot &out)
    {
        if (!query || (!engine && !memory))
        {
            out.gpu_status = engine_error.empty() ? "GPU counters unavailable" : engine_error;
            return;
        }
        const PDH_STATUS status = PdhCollectQueryData(query);
        if (status != ERROR_SUCCESS)
        {
            out.gpu_status = status_code("GPU sample unavailable", static_cast<unsigned long>(status));
            return;
        }
        const auto utilization = counter_value(engine, pid_prefix, false, &out);
        const auto dedicated = counter_value(memory, pid_prefix, true);
        out.gpu_available = utilization.available;
        if (out.gpu_available) out.gpu_engine_percent = std::clamp(utilization.value, 0.0, 100.0);
        out.gpu_memory_available = dedicated.available;
        if (out.gpu_memory_available) out.gpu_memory_mib = dedicated.value / kMib;
        out.gpu_status = out.gpu_available ? "busiest process engine; includes UI and inference" :
            engine_error.empty() ? utilization.detail : engine_error;
        if (!out.gpu_memory_available)
            out.gpu_status += "; dedicated memory N/A: " + (memory_error.empty() ? dedicated.detail : memory_error);
    }
};

struct SampleRange
{
    std::size_t count = 0;
    double first = 0.0, last = 0.0, minimum = 0.0, maximum = 0.0, total = 0.0;

    void add(bool available, double value)
    {
        if (!available || !std::isfinite(value) || value < 0.0) return;
        if (count == 0) first = minimum = maximum = value;
        last = value;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        total += value;
        ++count;
    }

    std::string utilization(const char *name) const
    {
        // The unsuffixed utilization is the sampled mean of this window.
        return std::string(" ") + name + "=" + measurement(count != 0, count == 0 ? 0.0 : total / static_cast<double>(count)) +
            " " + name + "_min=" + measurement(count != 0, minimum) +
            " " + name + "_max=" + measurement(count != 0, maximum);
    }

    std::string memory(const char *name) const
    {
        return std::string(" ") + name + "=" + measurement(count != 0, last) +
            " " + name + "_first=" + measurement(count != 0, first) +
            " " + name + "_peak=" + measurement(count != 0, maximum);
    }
};

std::uint32_t availability(const PerformanceSnapshot &sample)
{
    const bool fields[]{sample.cpu_available, sample.gpu_available, sample.gpu_memory_available,
        sample.memory_available, sample.private_commit_available, sample.page_faults_available,
        sample.page_fault_delta_available, sample.io_available, sample.io_rate_available,
        sample.gpu_compute_available, sample.gpu_graphics_available, sample.gpu_copy_available};
    std::uint32_t flags = 0;
    for (std::size_t index = 0; index < std::size(fields); ++index)
        if (fields[index]) flags |= std::uint32_t{1} << index;
    return flags;
}

struct ResourceWindow
{
    std::size_t samples = 0, fault_samples = 0, io_samples = 0;
    SampleRange cpu, gpu, compute, graphics, copy, rss, commit, gpu_memory, read_rate, write_rate;
    std::uint64_t faults = 0, read_bytes = 0, write_bytes = 0, read_ops = 0, write_ops = 0;
    PerformanceSnapshot last;

    void add(const PerformanceSnapshot &value)
    {
        ++samples;
        cpu.add(value.cpu_available, value.cpu_percent);
        gpu.add(value.gpu_available, value.gpu_engine_percent);
        compute.add(value.gpu_compute_available, value.gpu_compute_percent);
        graphics.add(value.gpu_graphics_available, value.gpu_graphics_percent);
        copy.add(value.gpu_copy_available, value.gpu_copy_percent);
        rss.add(value.memory_available, value.working_set_mib);
        commit.add(value.private_commit_available, value.private_commit_mib);
        gpu_memory.add(value.gpu_memory_available, value.gpu_memory_mib);
        read_rate.add(value.io_rate_available, value.io_read_mib_per_second);
        write_rate.add(value.io_rate_available, value.io_write_mib_per_second);
        if (value.page_fault_delta_available) { ++fault_samples; faults += value.page_faults_delta; }
        if (value.io_rate_available)
        {
            ++io_samples;
            read_bytes += value.io_read_bytes_delta;
            write_bytes += value.io_write_bytes_delta;
            read_ops += value.io_read_operations_delta;
            write_ops += value.io_write_operations_delta;
        }
        last = value;
    }

    bool quiet() const
    {
        return cpu.maximum < 2.0 && gpu.maximum < 25.0 && gpu.maximum - gpu.minimum < 2.0 &&
            rss.maximum - rss.minimum < 4.0 && commit.maximum - commit.minimum < 4.0 &&
            gpu_memory.maximum - gpu_memory.minimum < 8.0 && faults < 64 && read_bytes < 65536 && write_bytes < 65536;
    }

    std::string text(std::string_view reason, double duration_ms) const
    {
        const auto counter = [](bool valid, std::uint64_t value) { return valid ? std::to_string(value) : "N/A"; };
        return "event=window reason=" + std::string(reason) + " samples=" + std::to_string(samples) +
            " window_ms=" + measurement(true, duration_ms) + " cpu_samples=" + std::to_string(cpu.count) +
            " gpu_samples=" + std::to_string(gpu.count) + " memory_samples=" + std::to_string(rss.count) +
            " io_samples=" + std::to_string(io_samples) + cpu.utilization("cpu_percent") + gpu.utilization("gpu_engine_percent") +
            compute.utilization("gpu_compute_percent") + graphics.utilization("gpu_graphics_percent") +
            copy.utilization("gpu_copy_percent") + rss.memory("rss_mib") + commit.memory("private_commit_mib") +
            gpu_memory.memory("gpu_memory_mib") +
            " page_faults_total=" + counter(last.page_faults_available, last.page_faults_total) +
            " page_faults_delta=" + counter(fault_samples != 0, faults) + " page_faults_scope=soft_and_hard" +
            " io_read_bytes_total=" + counter(last.io_available, last.io_read_bytes) +
            " io_write_bytes_total=" + counter(last.io_available, last.io_write_bytes) +
            " io_read_ops_total=" + counter(last.io_available, last.io_read_operations) +
            " io_write_ops_total=" + counter(last.io_available, last.io_write_operations) +
            " io_read_bytes_delta=" + counter(io_samples != 0, read_bytes) +
            " io_write_bytes_delta=" + counter(io_samples != 0, write_bytes) +
            " io_read_ops_delta=" + counter(io_samples != 0, read_ops) +
            " io_write_ops_delta=" + counter(io_samples != 0, write_ops) +
            read_rate.utilization("io_read_mib_s") + write_rate.utilization("io_write_mib_s") +
            " cpu_scope=all_logical_processors gpu_scope=process_ui_and_inference io_scope=process_file_and_device" +
            " gpu_status=" + last.gpu_status;
    }
};

bool quiet_change(const PerformanceSnapshot &first, const PerformanceSnapshot &last)
{
    return availability(first) == availability(last) && first.gpu_status == last.gpu_status &&
        std::abs(first.cpu_percent - last.cpu_percent) < 1.0 && std::abs(first.gpu_engine_percent - last.gpu_engine_percent) < 2.0 &&
        std::abs(first.working_set_mib - last.working_set_mib) < 4.0 &&
        std::abs(first.private_commit_mib - last.private_commit_mib) < 4.0 &&
        std::abs(first.gpu_memory_mib - last.gpu_memory_mib) < 8.0;
}

bool resource_alarm(const PerformanceSnapshot &value)
{
    return (value.cpu_available && value.cpu_percent >= 80.0) || (value.gpu_available && value.gpu_engine_percent >= 95.0) ||
        (value.io_rate_available && (value.io_read_mib_per_second >= 100.0 || value.io_write_mib_per_second >= 100.0));
}

struct ResourceLogReducer
{
    ResourceWindow window;
    PerformanceSnapshot previous, emitted;
    bool has_previous = false, has_emitted = false, has_full_window = false;
    Clock::time_point window_started;

    explicit ResourceLogReducer(Clock::time_point started) : window_started(started) {}

    std::optional<std::string> flush(Clock::time_point now, std::string_view reason)
    {
        if (window.samples == 0) return std::nullopt;
        if (window.samples >= 5) has_full_window = true;
        auto text = window.text(reason, std::chrono::duration<double, std::milli>(now - window_started).count());
        emitted = window.last;
        has_emitted = true;
        window = {};
        window_started = now;
        return text;
    }

    std::optional<std::string> add(const PerformanceSnapshot &value, Clock::time_point now)
    {
        const bool changed = has_previous &&
            (availability(previous) != availability(value) || previous.gpu_status != value.gpu_status);
        const bool alarm_changed = has_previous && resource_alarm(previous) != resource_alarm(value);
        const bool memory_jump = has_previous &&
            ((value.memory_available && previous.memory_available && std::abs(value.working_set_mib - previous.working_set_mib) >= 64.0) ||
             (value.private_commit_available && previous.private_commit_available && std::abs(value.private_commit_mib - previous.private_commit_mib) >= 64.0) ||
             (value.gpu_memory_available && previous.gpu_memory_available && std::abs(value.gpu_memory_mib - previous.gpu_memory_mib) >= 64.0));
        window.add(value);
        previous = value;
        has_previous = true;
        if (!has_emitted) return flush(now, "initial_sample");
        if (changed || alarm_changed || memory_jump) return flush(now, changed ? "availability_changed" : "resource_change");
        const auto elapsed = now - window_started;
        if (elapsed >= std::chrono::seconds(5))
        {
            if (!has_full_window || !window.quiet() || !quiet_change(emitted, value)) return flush(now, "window");
            if (elapsed >= std::chrono::seconds(30)) return flush(now, "stable_heartbeat");
        }
        return std::nullopt;
    }
};
}

struct PerformanceSampler::Impl
{
    mutable std::mutex state_mutex;
    PerformanceSnapshot view;
    std::mutex lifecycle_mutex;
    std::mutex wait_mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread worker;
    Logger *logger = nullptr;

    void publish(PerformanceSnapshot sample)
    {
        std::lock_guard lock(state_mutex);
        view = std::move(sample);
    }

    void run() noexcept
    {
        try
        {
            if (logger) logger->write(LogLevel::Info, "perf", "sampling started; interval_ms=1000 pid=" +
                std::to_string(GetCurrentProcessId()) + " log_window_ms=5000 stable_heartbeat_ms=30000" +
                " cpu_scope=all_logical_processors gpu_scope=process_ui_and_inference");
            CpuReading previous;
            double ignored = 0.0;
            (void)cpu_reading(previous, ignored);
            GpuCounters gpu;
            gpu.initialize();
            ProcessReading process;
            PerformanceSnapshot baseline;
            process.sample(baseline);
            ResourceLogReducer logs(Clock::now());
            for (;;)
            {
                {
                    std::unique_lock lock(wait_mutex);
                    if (wake.wait_for(lock, std::chrono::seconds(1), [this] { return stopping; })) break;
                }
                PerformanceSnapshot sample;
                sample.cpu_available = cpu_reading(previous, sample.cpu_percent);
                process.sample(sample);
                gpu.sample(sample);
                const auto entry = logs.add(sample, Clock::now());
                if (entry && logger) logger->write(LogLevel::Info, "perf", *entry);
                publish(std::move(sample));
            }
            if (const auto entry = logs.flush(Clock::now(), "sampling_stopped"); entry && logger)
                logger->write(LogLevel::Info, "perf", *entry);
            if (logger) logger->write(LogLevel::Info, "perf", "sampling stopped");
        }
        catch (const std::exception &failure)
        {
            PerformanceSnapshot failed;
            failed.gpu_status = std::string("Performance sampler failed: ") + failure.what();
            if (logger) logger->write(LogLevel::Warning, "perf", failed.gpu_status);
            publish(std::move(failed));
        }
        catch (...)
        {
            PerformanceSnapshot failed;
            failed.gpu_status = "Performance sampler failed unexpectedly";
            if (logger) logger->write(LogLevel::Warning, "perf", failed.gpu_status);
            publish(std::move(failed));
        }
    }
};

PerformanceSampler::PerformanceSampler() : impl_(std::make_unique<Impl>())
{
    impl_->view.gpu_status = "Performance sampler is stopped";
}

PerformanceSampler::~PerformanceSampler() { stop(); }

void PerformanceSampler::start(Logger *logger)
{
    std::lock_guard lifecycle(impl_->lifecycle_mutex);
    if (impl_->worker.joinable()) return;
    {
        std::lock_guard lock(impl_->wait_mutex);
        impl_->stopping = false;
    }
    PerformanceSnapshot initial;
    initial.gpu_status = "GPU counters warming up; process includes UI and inference";
    impl_->publish(std::move(initial));
    impl_->logger = logger;
    try { impl_->worker = std::thread([impl = impl_.get()] { impl->run(); }); }
    catch (const std::exception &failure)
    {
        PerformanceSnapshot failed;
        failed.gpu_status = std::string("Cannot start performance sampling: ") + failure.what();
        if (logger) logger->write(LogLevel::Warning, "perf", failed.gpu_status);
        impl_->publish(std::move(failed));
        impl_->logger = nullptr;
    }
}

void PerformanceSampler::stop()
{
    std::lock_guard lifecycle(impl_->lifecycle_mutex);
    if (!impl_->worker.joinable()) return;
    {
        std::lock_guard lock(impl_->wait_mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    impl_->worker.join();
    impl_->logger = nullptr;
    PerformanceSnapshot stopped;
    stopped.gpu_status = "Performance sampling stopped; measurements unavailable";
    impl_->publish(std::move(stopped));
}

PerformanceSnapshot PerformanceSampler::snapshot() const
{
    std::lock_guard lock(impl_->state_mutex);
    return impl_->view;
}
}
