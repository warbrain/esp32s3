#include "check.hpp"

// Use the production aggregation and worker code without exposing PDH details
// or test-only hooks through the base library's public contract.
#define PerformanceSampler TestPerformanceSampler
#include "../src/base/performance.cpp"
#undef PerformanceSampler

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace
{
bool valid_performance(const hmi::base::PerformanceSnapshot &sample)
{
    return std::isfinite(sample.cpu_percent) && sample.cpu_percent >= 0.0 && sample.cpu_percent <= 100.0 &&
        std::isfinite(sample.gpu_engine_percent) && sample.gpu_engine_percent >= 0.0 && sample.gpu_engine_percent <= 100.0 &&
        std::isfinite(sample.working_set_mib) && sample.working_set_mib >= 0.0 &&
        std::isfinite(sample.gpu_memory_mib) && sample.gpu_memory_mib >= 0.0 &&
        (sample.cpu_available || sample.cpu_percent == 0.0) &&
        (sample.gpu_available || sample.gpu_engine_percent == 0.0) &&
        (sample.gpu_memory_available || sample.gpu_memory_mib == 0.0) &&
        std::isfinite(sample.private_commit_mib) && sample.private_commit_mib >= 0.0 &&
        std::isfinite(sample.io_read_mib_per_second) && sample.io_read_mib_per_second >= 0.0 &&
        std::isfinite(sample.io_write_mib_per_second) && sample.io_write_mib_per_second >= 0.0 &&
        (sample.private_commit_available || sample.private_commit_mib == 0.0) &&
        (sample.io_rate_available || (sample.io_read_mib_per_second == 0.0 && sample.io_write_mib_per_second == 0.0)) &&
        std::isfinite(sample.gpu_compute_percent) && sample.gpu_compute_percent >= 0.0 && sample.gpu_compute_percent <= 100.0 &&
        std::isfinite(sample.gpu_graphics_percent) && sample.gpu_graphics_percent >= 0.0 && sample.gpu_graphics_percent <= 100.0 &&
        std::isfinite(sample.gpu_copy_percent) && sample.gpu_copy_percent >= 0.0 && sample.gpu_copy_percent <= 100.0 &&
        (sample.gpu_compute_available || sample.gpu_compute_percent == 0.0) &&
        (sample.gpu_graphics_available || sample.gpu_graphics_percent == 0.0) &&
        (sample.gpu_copy_available || sample.gpu_copy_percent == 0.0);
}

bool wait_for_cpu(hmi::base::TestPerformanceSampler &sampler)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (sampler.snapshot().cpu_available) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return sampler.snapshot().cpu_available;
}

struct PerformanceLogDirectory
{
    std::filesystem::path parent = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
    std::filesystem::path path = parent / (L"hmi-perf-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    PerformanceLogDirectory()
    {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("cannot create performance log fixture");
    }
    ~PerformanceLogDirectory()
    {
        const auto checked = std::filesystem::absolute(path).lexically_normal();
        if (checked.parent_path() == parent && checked.filename().native().starts_with(L"hmi-perf-"))
        {
            std::error_code error;
            std::filesystem::remove_all(checked, error);
        }
    }
};
}

HMI_TEST(performance_gpu_aggregation_is_pid_scoped_and_uses_the_busiest_engine)
{
    wchar_t first[] = L"pid_42_luid_0x1_phys_0_eng_0_engtype_3D";
    wchar_t second[] = L"pid_42_luid_0x1_phys_0_eng_1_engtype_Compute";
    wchar_t other[] = L"pid_420_luid_0x1_phys_0_eng_0_engtype_3D";
    PDH_FMT_COUNTERVALUE_ITEM_W items[5]{};
    items[0].szName = first;
    items[0].FmtValue.CStatus = PDH_CSTATUS_VALID_DATA;
    items[0].FmtValue.doubleValue = 60.0;
    items[1].szName = second;
    items[1].FmtValue.CStatus = PDH_CSTATUS_NEW_DATA;
    items[1].FmtValue.doubleValue = 70.0;
    items[2].szName = other;
    items[2].FmtValue.CStatus = PDH_CSTATUS_VALID_DATA;
    items[2].FmtValue.doubleValue = 95.0;
    items[3].szName = first;
    items[3].FmtValue.CStatus = PDH_CSTATUS_INVALID_DATA;
    items[3].FmtValue.doubleValue = 99.0;
    items[4].szName = first;
    items[4].FmtValue.CStatus = PDH_CSTATUS_VALID_DATA;
    items[4].FmtValue.doubleValue = std::numeric_limits<double>::quiet_NaN();
    const auto busiest = hmi::base::aggregate_counter_items(items, 5, L"pid_42_", false);
    CHECK(busiest.available && busiest.value == 70.0);
    const auto memory = hmi::base::aggregate_counter_items(items, 5, L"pid_42_", true);
    CHECK(memory.available && memory.value == 130.0);
    const auto missing = hmi::base::aggregate_counter_items(items, 5, L"pid_43_", false);
    CHECK(!missing.available && missing.value == 0.0 && !missing.detail.empty());
    items[0].FmtValue.doubleValue = 0.0;
    const auto idle = hmi::base::aggregate_counter_items(items, 1, L"pid_42_", false);
    CHECK(idle.available && idle.value == 0.0);
    CHECK(hmi::base::measurement(false, 0.0) == "N/A");
    CHECK(hmi::base::measurement(true, 0.0) == "0.00");
}

HMI_TEST(performance_snapshot_starts_unavailable_and_is_a_detached_value)
{
    hmi::base::TestPerformanceSampler sampler;
    auto first = sampler.snapshot();
    CHECK(!first.cpu_available && !first.gpu_available && !first.gpu_memory_available);
    CHECK(valid_performance(first));
    CHECK(!first.gpu_status.empty());
    first.gpu_status = "local-only";
    CHECK(sampler.snapshot().gpu_status != first.gpu_status);
    sampler.stop();
    sampler.stop();
    CHECK(!sampler.snapshot().gpu_available);
}

HMI_TEST(performance_sampling_is_idempotent_restartable_and_safe_for_concurrent_snapshots)
{
    hmi::base::TestPerformanceSampler sampler;
    sampler.start();
    sampler.start();
    std::atomic<bool> reading{true};
    std::atomic<bool> valid{true};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index)
        readers.emplace_back([&] {
            while (reading.load())
            {
                if (!valid_performance(sampler.snapshot())) valid.store(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    CHECK(wait_for_cpu(sampler));
    sampler.start();
    CHECK(sampler.snapshot().cpu_available);
    sampler.stop();
    sampler.stop();
    reading.store(false);
    for (auto &reader : readers) reader.join();
    CHECK(valid.load());
    const auto stopped = sampler.snapshot();
    CHECK(!stopped.cpu_available && !stopped.gpu_available && !stopped.gpu_memory_available);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(sampler.snapshot().gpu_status == stopped.gpu_status);
    sampler.start();
    CHECK(wait_for_cpu(sampler));
    CHECK(valid_performance(sampler.snapshot()));
    sampler.stop();
}

HMI_TEST(performance_logs_scope_values_and_unknown_measurements_without_owning_the_logger)
{
    PerformanceLogDirectory directory;
    hmi::base::Logger logger;
    hmi::base::LoggerOptions options;
    options.file = directory.path / "performance.log";
    CHECK(logger.configure(options));
    hmi::base::TestPerformanceSampler sampler;
    sampler.start(&logger);
    sampler.start(nullptr);
    CHECK(wait_for_cpu(sampler));
    sampler.stop();
    logger.flush();
    std::ifstream input(options.file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    CHECK(text.find("[perf] sampling started;") != std::string::npos);
    CHECK(text.find("cpu_percent=") != std::string::npos);
    CHECK(text.find("gpu_engine_percent=") != std::string::npos);
    CHECK(text.find("rss_mib=") != std::string::npos);
    CHECK(text.find("gpu_memory_mib=") != std::string::npos);
    CHECK(text.find("gpu_scope=process_ui_and_inference") != std::string::npos);
    CHECK(text.find("[perf] sampling stopped") != std::string::npos);
    CHECK(logger.ready());
}

HMI_TEST(performance_engine_categories_are_separate_and_unknown_is_not_zero)
{
    wchar_t graphics_name[] = L"pid_42_luid_0x1_phys_0_eng_0_engtype_3D";
    wchar_t compute_name[] = L"pid_42_luid_0x1_phys_0_eng_1_engtype_Compute_0";
    wchar_t copy_name[] = L"pid_42_luid_0x1_phys_0_eng_2_engtype_Copy";
    PDH_FMT_COUNTERVALUE_ITEM_W items[3]{};
    for (auto &item : items) item.FmtValue.CStatus = PDH_CSTATUS_VALID_DATA;
    items[0].szName = graphics_name; items[0].FmtValue.doubleValue = 11.0;
    items[1].szName = compute_name; items[1].FmtValue.doubleValue = 63.0;
    items[2].szName = copy_name; items[2].FmtValue.doubleValue = 0.0;
    const auto compute = hmi::base::aggregate_counter_items(items, 3, L"pid_42_", false, hmi::base::EngineGroup::Compute);
    const auto graphics = hmi::base::aggregate_counter_items(items, 3, L"pid_42_", false, hmi::base::EngineGroup::Graphics);
    const auto copy = hmi::base::aggregate_counter_items(items, 3, L"pid_42_", false, hmi::base::EngineGroup::Copy);
    const auto missing = hmi::base::aggregate_counter_items(items, 1, L"pid_42_", false, hmi::base::EngineGroup::Compute);
    CHECK(compute.available && compute.value == 63.0);
    CHECK(graphics.available && graphics.value == 11.0);
    CHECK(copy.available && copy.value == 0.0);
    CHECK(!missing.available && missing.value == 0.0);
}

HMI_TEST(performance_windows_preserve_spikes_and_stable_windows_become_heartbeats)
{
    using namespace std::chrono;
    const hmi::base::Clock::time_point start{};
    hmi::base::ResourceLogReducer reducer(start);
    hmi::base::PerformanceSnapshot sample;
    sample.cpu_available = sample.gpu_available = sample.memory_available = sample.private_commit_available = true;
    sample.gpu_memory_available = sample.io_available = sample.io_rate_available = sample.page_faults_available = true;
    sample.page_fault_delta_available = true;
    sample.cpu_percent = 0.3;
    sample.gpu_engine_percent = 11.0;
    sample.working_set_mib = 1000.0;
    sample.private_commit_mib = 1800.0;
    sample.gpu_memory_mib = 3000.0;
    sample.gpu_status = "steady";
    auto entry = reducer.add(sample, start + seconds(1));
    CHECK(entry && entry->find("reason=initial_sample") != std::string::npos);
    for (int second = 2; second < 6; ++second)
        CHECK(!reducer.add(sample, start + seconds(second)));
    entry = reducer.add(sample, start + seconds(6));
    CHECK(entry && entry->find("samples=5") != std::string::npos);
    for (int second = 7; second < 36; ++second)
        CHECK(!reducer.add(sample, start + seconds(second)));
    entry = reducer.add(sample, start + seconds(36));
    CHECK(entry && entry->find("reason=stable_heartbeat samples=30") != std::string::npos);
    sample.cpu_percent = 90.0;
    entry = reducer.add(sample, start + seconds(37));
    CHECK(entry && entry->find("reason=resource_change") != std::string::npos);
    CHECK(entry && entry->find("cpu_percent_max=90.00") != std::string::npos);
    sample.cpu_percent = 0.3;
    CHECK(reducer.add(sample, start + seconds(38)).has_value());
    sample.gpu_available = false;
    sample.gpu_engine_percent = 0.0;
    sample.gpu_status = "unavailable";
    entry = reducer.add(sample, start + seconds(39));
    CHECK(entry && entry->find("reason=availability_changed") != std::string::npos);
    CHECK(entry && entry->find("gpu_engine_percent=N/A") != std::string::npos);
    CHECK(!reducer.add(sample, start + seconds(40)));
    entry = reducer.flush(start + seconds(41), "stop");
    CHECK(entry && entry->find("reason=stop samples=1") != std::string::npos);
    CHECK(!reducer.flush(start + seconds(42), "stop"));
}

HMI_TEST(performance_window_reports_range_deltas_and_soft_plus_hard_fault_scope)
{
    hmi::base::ResourceWindow window;
    hmi::base::PerformanceSnapshot first;
    first.cpu_available = first.memory_available = first.io_available = first.io_rate_available = true;
    first.page_fault_delta_available = first.page_faults_available = true;
    first.cpu_percent = 10.0; first.working_set_mib = 100.0;
    first.io_read_bytes_delta = 100; first.io_write_bytes_delta = 50;
    first.io_read_operations_delta = 2; first.io_write_operations_delta = 1;
    first.page_faults_delta = 3; first.page_faults_total = 100;
    window.add(first);
    auto last = first;
    last.cpu_percent = 50.0; last.working_set_mib = 120.0;
    last.io_read_bytes_delta = 200; last.io_write_bytes_delta = 80;
    last.page_faults_delta = 5; last.page_faults_total = 105;
    window.add(last);
    const auto text = window.text("test", 2000.0);
    CHECK(text.find("cpu_percent=30.00 cpu_percent_min=10.00 cpu_percent_max=50.00") != std::string::npos);
    CHECK(text.find("rss_mib=120.00 rss_mib_first=100.00 rss_mib_peak=120.00") != std::string::npos);
    CHECK(text.find("io_read_bytes_delta=300") != std::string::npos);
    CHECK(text.find("io_write_bytes_delta=130") != std::string::npos);
    CHECK(text.find("page_faults_total=105 page_faults_delta=8 page_faults_scope=soft_and_hard") != std::string::npos);
    CHECK(text.find("private_commit_mib=N/A") != std::string::npos);
}
