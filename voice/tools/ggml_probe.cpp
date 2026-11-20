#include <ggml.h>
#include <ggml-alloc.h>
#include <ggml-backend.h>

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "ggml_probe_policy.hpp"

namespace
{
std::string quote(std::string_view text)
{
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : text)
    {
        if (ch == '"' || ch == '\\') { out += '\\'; out += static_cast<char>(ch); }
        else if (ch < 32) { out += "\\u00"; out += hex[ch >> 4]; out += hex[ch & 15]; }
        else out += static_cast<char>(ch);
    }
    return out + '"';
}

std::string utf8(const std::filesystem::path &path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::filesystem::path executable_directory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length == buffer.size()) throw std::runtime_error("cannot locate probe executable");
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::string environment(const char *name)
{
    char *value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0) return {};
    std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
    return value ? value : "";
}

std::string hardware_features()
{
    int leaf[4]{};
    __cpuid(leaf, 0);
    const int maximum = leaf[0];
    __cpuidex(leaf, 1, 0);
    const unsigned int ecx = static_cast<unsigned int>(leaf[2]);
    const bool xsave = (ecx & (1u << 27)) != 0;
    const auto xcr = xsave ? _xgetbv(0) : 0;
    const bool avx_state = (xcr & 6) == 6;
    const bool avx = avx_state && (ecx & (1u << 28));
    const bool avx512_state = avx_state && (xcr & 0xe0) == 0xe0;
    int seven[4]{};
    if (maximum >= 7) __cpuidex(seven, 7, 0);
    const auto ebx7 = static_cast<unsigned int>(seven[1]);
    const auto ecx7 = static_cast<unsigned int>(seven[2]);
    std::string out = "{";
    const auto add = [&out](const char *name, bool supported) {
        if (out.size() > 1) out += ',';
        out += quote(name) + ':' + (supported ? "true" : "false");
    };
    add("SSE42", (ecx & (1u << 20)) != 0);
    add("AVX", avx);
    add("FMA", avx && (ecx & (1u << 12)));
    add("F16C", avx && (ecx & (1u << 29)));
    add("AVX2", avx && (ebx7 & (1u << 5)));
    add("BMI2", (ebx7 & (1u << 8)) != 0);
    add("AVX512", avx512_state && (ebx7 & (1u << 16)));
    add("AVX512_VBMI", avx512_state && (ecx7 & (1u << 1)));
    add("AVX512_VNNI", avx512_state && (ecx7 & (1u << 11)));
    int seven_one[4]{};
    if (maximum >= 7 && seven[0] >= 1) __cpuidex(seven_one, 7, 1);
    add("AVX_VNNI", avx && (seven_one[0] & (1 << 4)));
    return out + '}';
}

// Upstream builds this entry point without ISA-specific flags. A zero score
// means incompatible; it is a compatibility ranking, never a timing result.
int cpu_score(const std::filesystem::path &path)
{
    const HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return -1;
    const auto symbol = GetProcAddress(module, "ggml_backend_score");
    int (*score)() = nullptr;
    static_assert(sizeof(score) == sizeof(symbol));
    std::memcpy(&score, &symbol, sizeof(score));
    const int result = score ? score() : -1;
    FreeLibrary(module);
    return result;
}

std::uint64_t enabled_xstate()
{
    int leaf[4]{};
    __cpuidex(leaf, 1, 0);
    return (leaf[2] & (1 << 27)) ? _xgetbv(0) : 0;
}

std::vector<std::filesystem::path> cpu_libraries(const std::filesystem::path &directory)
{
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        const auto name = entry.path().filename().string();
        if ((name.starts_with("ggml-cpu-") || name == "ggml-cpu.dll") && entry.path().extension() == ".dll") paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

struct Options
{
    bool inventory = false;
    std::string backend = "CPU";
    std::string variant = "auto";
    std::string op = "matmul";
    int threads = 1;
    int size = 256;
    int runs = 5;
};

int integer(const std::string &text, int minimum, int maximum)
{
    std::size_t consumed = 0;
    const int result = std::stoi(text, &consumed);
    if (consumed != text.size() || result < minimum || result > maximum)
        throw std::runtime_error("numeric option outside allowed range");
    return result;
}

Options options(int argc, char **argv)
{
    if (argc < 2) throw std::runtime_error("usage: hmi_ggml_probe inventory|perf [--backend CPU|CUDA0|Vulkan0] [--variant auto|ggml-cpu-haswell.dll] [--threads 1..256] [--op matmul|softmax] [--size 32..1024] [--runs 1..100]");
    Options out;
    out.inventory = std::string_view(argv[1]) == "inventory";
    if (!out.inventory && std::string_view(argv[1]) != "perf") throw std::runtime_error("mode must be inventory or perf");
    for (int index = 2; index < argc; index += 2)
    {
        if (index + 1 == argc) throw std::runtime_error("missing option value");
        const std::string key = argv[index];
        const std::string value = argv[index + 1];
        if (key == "--backend") out.backend = value;
        else if (key == "--variant") out.variant = value;
        else if (key == "--op") out.op = value;
        else if (key == "--threads") out.threads = integer(value, 1, 256);
        else if (key == "--size") out.size = integer(value, 32, 1024);
        else if (key == "--runs") out.runs = integer(value, 1, 100);
        else throw std::runtime_error("unknown option: " + key);
    }
    if (out.op != "matmul" && out.op != "softmax") throw std::runtime_error("op must be matmul or softmax");
    if (out.variant != "auto" && (out.backend != "CPU" ||
        out.variant.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.") != std::string::npos ||
        (!out.variant.starts_with("ggml-cpu-") && out.variant != "ggml-cpu.dll") || !out.variant.ends_with(".dll")))
        throw std::runtime_error("variant must be auto or a CPU DLL filename in the probe directory");
    return out;
}

void load_backends(const Options &config, const std::filesystem::path &directory)
{
    // Ignore out-of-tree plugins to keep the tested SDK set reproducible.
    _putenv_s("GGML_BACKEND_PATH", "");
    // The pinned upstream score checks CPUID but omits OS XSAVE support. Do
    // not call load_all(), which could initialize an unsafe highest-score CPU.
    std::filesystem::path selected;
    const auto xstate = enabled_xstate();
    if (config.variant == "auto")
    {
        int best = 0;
        for (const auto &path : cpu_libraries(directory))
        {
            const int score = cpu_score(path);
            if (score > best && hmi::diagnostics::cpu_score_os_safe(score, xstate)) { best = score; selected = path; }
        }
        for (const char *name : {"ggml-cuda.dll", "ggml-vulkan.dll"})
        {
            const auto path = directory / name;
            if (std::filesystem::exists(path)) ggml_backend_load(utf8(path).c_str());
        }
    }
    else
    {
        selected = directory / config.variant;
        if (!hmi::diagnostics::cpu_score_os_safe(cpu_score(selected), xstate))
            throw std::runtime_error("requested CPU variant is missing or incompatible with hardware/OS ISA state");
    }
    if (!selected.empty() && !ggml_backend_load(utf8(selected).c_str())) throw std::runtime_error("selected CPU variant failed to register");
}

std::string backend_features(ggml_backend_reg_t registry)
{
    const auto function = reinterpret_cast<ggml_backend_get_features_t>(ggml_backend_reg_get_proc_address(registry, "ggml_backend_get_features"));
    std::string out = "{";
    if (function)
        for (const auto *feature = function(registry); feature && feature->name; ++feature)
        {
            if (out.size() > 1) out += ',';
            out += quote(feature->name) + ':' + quote(feature->value ? feature->value : "");
        }
    return out + '}';
}

std::string backend_library(ggml_backend_reg_t registry)
{
    const auto *address = ggml_backend_reg_get_proc_address(registry, "ggml_backend_get_features");
    HMODULE module = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(address), &module)) return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return {};
    path.resize(length);
    return utf8(std::filesystem::path(path).filename());
}

bool backend_openmp(ggml_backend_reg_t registry)
{
    const auto function = reinterpret_cast<ggml_backend_get_features_t>(ggml_backend_reg_get_proc_address(registry, "ggml_backend_get_features"));
    if (function)
        for (const auto *feature = function(registry); feature && feature->name; ++feature)
            if (std::string_view(feature->name) == "OPENMP" && feature->value && std::string_view(feature->value) == "1") return true;
    return false;
}

void inventory(const std::filesystem::path &directory, std::ostream &report)
{
    report << "{\"schema\":1,\"status\":\"ok\",\"logical_processors\":" << std::thread::hardware_concurrency()
              << ",\"hardware_isa_os_enabled\":" << hardware_features() << ",\"devices\":[";
    for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index)
    {
        const auto device = ggml_backend_dev_get(index);
        if (index) report << ',';
        report << "{\"name\":" << quote(ggml_backend_dev_name(device)) << ",\"description\":"
                  << quote(ggml_backend_dev_description(device)) << ",\"cpu\":"
                  << (ggml_backend_dev_type(device) == GGML_BACKEND_DEVICE_TYPE_CPU ? "true" : "false")
                  << ",\"library\":" << quote(backend_library(ggml_backend_dev_backend_reg(device)))
                  << ",\"openmp_enabled\":" << (backend_openmp(ggml_backend_dev_backend_reg(device)) ? "true" : "false")
                  << ",\"features\":" << backend_features(ggml_backend_dev_backend_reg(device)) << '}';
    }
    report << "],\"cpu_variants\":[";
    const auto variants = cpu_libraries(directory);
    for (std::size_t index = 0; index < variants.size(); ++index)
    {
        const int score = cpu_score(variants[index]);
        if (index) report << ',';
        report << "{\"file\":" << quote(variants[index].filename().string()) << ",\"compatibility_score\":" << score
                  << ",\"compatible\":" << (hmi::diagnostics::cpu_score_os_safe(score, enabled_xstate()) ? "true" : "false") << '}';
    }
    report << "]}\n";
}

void benchmark(const Options &config, std::ostream &report)
{
    const auto device = ggml_backend_dev_by_name(config.backend.c_str());
    if (!device) throw std::runtime_error("requested device not found: " + config.backend);
    using Backend = std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)>;
    Backend backend(ggml_backend_dev_init(device, nullptr), &ggml_backend_free);
    if (!backend) throw std::runtime_error("backend initialization failed");
    const auto registry = ggml_backend_dev_backend_reg(device);
    const auto set_threads = reinterpret_cast<ggml_backend_set_n_threads_t>(ggml_backend_reg_get_proc_address(registry, "ggml_backend_set_n_threads"));
    const bool cpu = ggml_backend_dev_type(device) == GGML_BACKEND_DEVICE_TYPE_CPU;
    if (cpu && !set_threads) throw std::runtime_error("CPU backend does not expose thread configuration");
    if (cpu) set_threads(backend.get(), config.threads);
    const ggml_init_params parameters{ggml_tensor_overhead() * 8 + ggml_graph_overhead_custom(32, false), nullptr, true};
    std::unique_ptr<ggml_context, decltype(&ggml_free)> context(ggml_init(parameters), &ggml_free);
    if (!context) throw std::runtime_error("ggml context allocation failed");
    const auto side = static_cast<int64_t>(config.size);
    auto *a = ggml_new_tensor_2d(context.get(), GGML_TYPE_F32, side, side);
    auto *b = config.op == "matmul" ? ggml_new_tensor_2d(context.get(), GGML_TYPE_F32, side, side) : nullptr;
    auto *out = b ? ggml_mul_mat(context.get(), a, b) : ggml_soft_max(context.get(), a);
    if (!ggml_backend_supports_op(backend.get(), out)) throw std::runtime_error("operation unsupported by requested device");
    auto *graph = ggml_new_graph_custom(context.get(), 32, false);
    ggml_build_forward_expand(graph, out);
    std::unique_ptr<ggml_backend_buffer, decltype(&ggml_backend_buffer_free)> buffer(
        ggml_backend_alloc_ctx_tensors(context.get(), backend.get()), &ggml_backend_buffer_free);
    if (!buffer) throw std::runtime_error("device tensor allocation failed");
    // Constant inputs provide a cheap independent result check on every cell.
    const auto elements = static_cast<std::size_t>(side * side);
    std::vector<float> input(elements, 0.5f);
    ggml_backend_tensor_set(a, input.data(), 0, input.size() * sizeof(float));
    if (b)
    {
        std::fill(input.begin(), input.end(), 0.25f);
        ggml_backend_tensor_set(b, input.data(), 0, input.size() * sizeof(float));
    }
    const auto compute = [&] {
        if (ggml_backend_graph_compute(backend.get(), graph) != GGML_STATUS_SUCCESS)
            throw std::runtime_error("ggml_backend_graph_compute failed");
        ggml_backend_synchronize(backend.get());
    };
    compute();
    std::vector<double> times;
    for (int run = 0; run < config.runs; ++run)
    {
        const auto start = std::chrono::steady_clock::now();
        compute();
        times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    ggml_backend_tensor_get(out, input.data(), 0, input.size() * sizeof(float));
    const float expected = b ? static_cast<float>(side) * 0.125f : 1.0f / static_cast<float>(side);
    const bool correct = std::all_of(input.begin(), input.end(), [expected](float value) {
        return std::isfinite(value) && std::abs(value - expected) <= std::max(1e-5f, std::abs(expected) * 1e-4f);
    });
    if (!correct) throw std::runtime_error("computed output failed independent numeric validation");
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];
    if (!std::isfinite(median) || median <= 0) throw std::runtime_error("invalid timing measurement");
    report << "{\"schema\":1,\"status\":\"measured\",\"backend\":" << quote(config.backend)
              << ",\"description\":" << quote(ggml_backend_dev_description(device))
              << ",\"variant\":" << quote(config.variant) << ",\"library\":" << quote(backend_library(registry))
              << ",\"features\":" << backend_features(registry)
              << ",\"openmp_enabled\":" << (backend_openmp(registry) ? "true" : "false")
              << ",\"operation\":" << quote(config.op) << ",\"dtype\":\"f32\",\"size\":" << config.size
              << ",\"cpu_threads\":" << (cpu ? config.threads : 0) << ",\"omp_num_threads\":" << quote(environment("OMP_NUM_THREADS"))
              << ",\"omp_wait_policy\":" << quote(environment("OMP_WAIT_POLICY"))
              << ",\"runs\":" << config.runs << ",\"warmup_runs\":1,\"correct\":true,\"median_us\":" << median
              << ",\"minimum_us\":" << times.front();
    if (b) report << ",\"gflops\":" << 2.0 * static_cast<double>(side * side * side) / median / 1000.0;
    report << "}\n";
}
}

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    std::ostringstream report;
    report.imbue(std::locale::classic());
    const auto emit = [&report] {
        const auto text = report.str();
        DWORD written = 0;
        return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    };
    try
    {
        const auto config = options(argc, argv);
        const auto directory = executable_directory();
        load_backends(config, directory);
        if (config.inventory) inventory(directory, report);
        else benchmark(config, report);
        return emit() ? 0 : 1;
    }
    catch (const std::exception &exception)
    {
        report.str({});
        report.clear();
        report << "{\"schema\":1,\"status\":\"failed\",\"error\":" << quote(exception.what()) << "}\n";
        emit();
        return 1;
    }
}

