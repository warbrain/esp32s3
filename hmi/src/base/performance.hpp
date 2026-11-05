#pragma once

#include <memory>
#include <cstdint>
#include <string>

namespace hmi::base
{
class Logger;

struct PerformanceSnapshot
{
    bool cpu_available = false;
    double cpu_percent = 0.0;
    bool gpu_available = false;
    double gpu_engine_percent = 0.0;
    bool gpu_memory_available = false;
    double gpu_memory_mib = 0.0;
    double working_set_mib = 0.0;
    std::string gpu_status;
    bool memory_available = false;
    bool private_commit_available = false;
    double private_commit_mib = 0.0;
    bool page_faults_available = false;
    bool page_fault_delta_available = false;
    std::uint64_t page_faults_total = 0; // Process soft and hard faults combined.
    std::uint64_t page_faults_delta = 0;
    bool io_available = false; // Process I/O, including files/devices; not disk-only.
    bool io_rate_available = false;
    std::uint64_t io_read_bytes = 0;
    std::uint64_t io_write_bytes = 0;
    std::uint64_t io_read_operations = 0;
    std::uint64_t io_write_operations = 0;
    std::uint64_t io_read_bytes_delta = 0;
    std::uint64_t io_write_bytes_delta = 0;
    std::uint64_t io_read_operations_delta = 0;
    std::uint64_t io_write_operations_delta = 0;
    double io_read_mib_per_second = 0.0;
    double io_write_mib_per_second = 0.0;
    bool gpu_compute_available = false;
    bool gpu_graphics_available = false;
    bool gpu_copy_available = false;
    double gpu_compute_percent = 0.0;
    double gpu_graphics_percent = 0.0;
    double gpu_copy_percent = 0.0;
};

// Samples this process, including UI and inference work. GPU utilization is
// the busiest reported engine, not a sum across engines or an ASR-only metric.
// Availability flags distinguish missing data from a measured zero.
// Snapshots update every second. File output aggregates five-second windows;
// stable quiet windows become thirty-second heartbeats without discarding peaks.
class PerformanceSampler
{
public:
    PerformanceSampler();
    ~PerformanceSampler();
    PerformanceSampler(const PerformanceSampler &) = delete;
    PerformanceSampler &operator=(const PerformanceSampler &) = delete;

    // Logger is borrowed until stop() returns and must outlive the sampler.
    // Repeated start calls leave the current worker and logger unchanged.
    void start(Logger *logger = nullptr);
    void stop();
    PerformanceSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
