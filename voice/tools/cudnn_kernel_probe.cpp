#include "engine/sdk_library.hpp"

#include <ggml.h>
#include <ggml-alloc.h>
#include <ggml-backend.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace hmi::diagnostics
{
namespace
{
using Clock = std::chrono::steady_clock;

std::string utf8(const std::filesystem::path &path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

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

bool emit(std::string_view text)
{
    DWORD written = 0;
    return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()),
        &written, nullptr) && written == text.size();
}

std::filesystem::path executable_directory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count == buffer.size()) throw std::runtime_error("cannot locate probe executable");
    buffer.resize(count);
    return std::filesystem::path(buffer).parent_path();
}

struct Options
{
    std::filesystem::path sdk = executable_directory() / "native";
    bool bias = false;
    bool help = false;
};

Options options(int argc, wchar_t **argv)
{
    Options config;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring key = argv[index];
        if (key == L"--help") config.help = true;
        else if (key == L"--bias") config.bias = true;
        else if (key == L"--sdk-dir")
        {
            if (++index == argc) throw std::runtime_error("--sdk-dir requires an absolute SDK bin directory");
            config.sdk = std::filesystem::path(argv[index]);
            if (!config.sdk.is_absolute()) throw std::runtime_error("--sdk-dir must be absolute");
        }
        else throw std::runtime_error("unknown option");
    }
    return config;
}

class Deadline
{
public:
    Deadline() : worker_([this] {
        std::unique_lock lock(mutex_);
        if (condition_.wait_for(lock, std::chrono::seconds(30), [this] { return complete_; })) return;
        emit("{\"schema\":1,\"status\":\"failed\",\"error\":\"cuDNN kernel probe exceeded 30 seconds\"}\n");
        TerminateProcess(GetCurrentProcess(), 124);
    }) {}

    ~Deadline()
    {
        { std::lock_guard lock(mutex_); complete_ = true; }
        condition_.notify_one();
        worker_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool complete_ = false;
    std::thread worker_;
};

class Module
{
public:
    explicit Module(const std::filesystem::path &path)
    {
        std::string error;
        handle_ = engine::detail::load_sdk_library(path, &error);
        if (!handle_) throw std::runtime_error(error);
    }
    ~Module() { if (handle_) FreeLibrary(handle_); }
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;
    HMODULE get() const noexcept { return handle_; }
private:
    HMODULE handle_ = nullptr;
};

template<class Function>
Function bind(HMODULE base, HMODULE core, const char *name)
{
    FARPROC address = GetProcAddress(base, name);
    if (!address) address = GetProcAddress(core, name);
    if (!address) throw std::runtime_error(std::string("missing matching ggml export: ") + name);
    Function result = nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

#define HMI_CUDNN_GGML_FUNCTIONS(X) \
    X(ggml_init) X(ggml_free) X(ggml_tensor_overhead) X(ggml_graph_overhead_custom) \
    X(ggml_new_tensor_4d) X(ggml_conv_2d_direct) X(ggml_conv_2d_direct_set_cudnn) \
    X(ggml_conv_2d_direct_set_cudnn_bias) X(ggml_new_graph_custom) X(ggml_build_forward_expand) \
    X(ggml_fp32_to_fp16) X(ggml_fp16_to_fp32) X(ggml_log_set) \
    X(ggml_backend_load) X(ggml_backend_unload) X(ggml_backend_reg_dev_count) X(ggml_backend_reg_dev_get) \
    X(ggml_backend_dev_name) X(ggml_backend_dev_init) X(ggml_backend_free) X(ggml_backend_supports_op) \
    X(ggml_backend_alloc_ctx_tensors) X(ggml_backend_buffer_free) X(ggml_backend_tensor_set) \
    X(ggml_backend_tensor_get) X(ggml_backend_graph_compute) X(ggml_backend_synchronize)

struct Api
{
#define HMI_DECLARE(name) decltype(&::name) name = nullptr;
    HMI_CUDNN_GGML_FUNCTIONS(HMI_DECLARE)
#undef HMI_DECLARE
    Api(HMODULE base, HMODULE core)
    {
#define HMI_BIND(name) name = bind<decltype(name)>(base, core, #name);
        HMI_CUDNN_GGML_FUNCTIONS(HMI_BIND)
#undef HMI_BIND
    }
};
#undef HMI_CUDNN_GGML_FUNCTIONS

struct Registration
{
    const Api &api;
    ggml_backend_reg_t value;
    ~Registration() { if (value) api.ggml_backend_unload(value); }
};

std::string convolution(const Api &api, ggml_backend_t backend, bool with_bias)
{
    const ggml_init_params parameters{
        api.ggml_tensor_overhead() * 8 + api.ggml_graph_overhead_custom(32, false), nullptr, true};
    std::unique_ptr<ggml_context, decltype(api.ggml_free)> context(api.ggml_init(parameters), api.ggml_free);
    if (!context) throw std::runtime_error("ggml context allocation failed");
    auto *weights = api.ggml_new_tensor_4d(context.get(), GGML_TYPE_F16, 1, 1, 8, 8);
    auto *input = api.ggml_new_tensor_4d(context.get(), GGML_TYPE_F16, 8, 8, 8, 1);
    auto *output = api.ggml_conv_2d_direct(context.get(), weights, input, 1, 1, 0, 0, 1, 1);
    api.ggml_conv_2d_direct_set_cudnn(output);
    ggml_tensor *bias = nullptr;
    if (with_bias)
    {
        bias = api.ggml_new_tensor_4d(context.get(), GGML_TYPE_F16, 8, 1, 1, 1);
        api.ggml_conv_2d_direct_set_cudnn_bias(output, bias);
    }
    if (output->type != GGML_TYPE_F16 || !api.ggml_backend_supports_op(backend, output))
        throw std::runtime_error("CUDA backend does not support requested F16 cuDNN convolution");
    auto *graph = api.ggml_new_graph_custom(context.get(), 32, false);
    api.ggml_build_forward_expand(graph, output);
    std::unique_ptr<ggml_backend_buffer, decltype(api.ggml_backend_buffer_free)> buffer(
        api.ggml_backend_alloc_ctx_tensors(context.get(), backend), api.ggml_backend_buffer_free);
    if (!buffer) throw std::runtime_error("CUDA tensor allocation failed");
    const std::vector<ggml_fp16_t> weight_values(64, api.ggml_fp32_to_fp16(0.5f));
    const std::vector<ggml_fp16_t> input_values(512, api.ggml_fp32_to_fp16(0.25f));
    api.ggml_backend_tensor_set(weights, weight_values.data(), 0, weight_values.size() * sizeof(ggml_fp16_t));
    api.ggml_backend_tensor_set(input, input_values.data(), 0, input_values.size() * sizeof(ggml_fp16_t));
    if (bias)
    {
        const std::vector<ggml_fp16_t> values(8, api.ggml_fp32_to_fp16(0.5f));
        api.ggml_backend_tensor_set(bias, values.data(), 0, values.size() * sizeof(ggml_fp16_t));
    }
    const auto compute = [&] {
        if (api.ggml_backend_graph_compute(backend, graph) != GGML_STATUS_SUCCESS)
            throw std::runtime_error("cuDNN convolution graph failed");
        api.ggml_backend_synchronize(backend);
    };
    compute();
    const auto start = Clock::now();
    compute();
    const double elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    std::vector<ggml_fp16_t> actual(512);
    api.ggml_backend_tensor_get(output, actual.data(), 0, actual.size() * sizeof(ggml_fp16_t));
    const float expected = with_bias ? 1.5f : 1.0f;
    float maximum_error = 0.0f;
    for (const auto value : actual)
    {
        const float converted = api.ggml_fp16_to_fp32(value);
        if (!std::isfinite(converted)) throw std::runtime_error("cuDNN convolution returned a non-finite value");
        maximum_error = std::max(maximum_error, std::abs(converted - expected));
    }
    if (maximum_error > 0.001f) throw std::runtime_error("cuDNN convolution failed independent numeric validation");
    if (!std::isfinite(elapsed) || elapsed <= 0) throw std::runtime_error("invalid cuDNN kernel timing");
    std::ostringstream report;
    report.imbue(std::locale::classic());
    report << "{\"bias\":" << (with_bias ? "true" : "false") << ",\"correct\":true,\"elements\":512,\"expected\":"
           << expected << ",\"maximum_absolute_error\":" << maximum_error << ",\"warmup_runs\":1,\"runs\":1,\"time_us\":" << elapsed << '}';
    return report.str();
}

std::string module_path(const wchar_t *name)
{
    const HMODULE module = GetModuleHandleW(name);
    if (!module) return {};
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size()) return {};
    path.resize(count);
    return utf8(std::filesystem::path(path));
}

std::string loaded_cudnn_modules(const std::filesystem::path &directory)
{
    std::string report = "[";
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        const auto name = engine::detail::sdk_fold_path(entry.path().filename());
        if (entry.is_regular_file() && name.starts_with(L"cudnn") && name.ends_with(L".dll"))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto &file : files)
    {
        const auto loaded = module_path(file.filename().c_str());
        if (loaded.empty()) continue;
        const auto actual = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t *>(loaded.data()), loaded.size()));
        if (engine::detail::sdk_fold_path(actual) != engine::detail::sdk_fold_path(file))
            throw std::runtime_error("cuDNN component loaded outside the selected SDK: " + loaded);
        if (report.size() > 1) report += ',';
        report += quote(loaded);
    }
    return report + ']';
}
}

int run(int argc, wchar_t **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    try
    {
        const auto config = options(argc, argv);
        if (config.help)
        {
            return emit("{\"schema\":1,\"status\":\"help\",\"usage\":\"hmi_cudnn_kernel_probe [--sdk-dir ABSOLUTE_BIN_DIRECTORY] [--bias]\","
                "\"operation\":\"F16 1x1 Conv2D with ggml_conv_2d_direct_set_cudnn; optional fused bias\",\"timeout_seconds\":30}\n") ? 0 : 1;
        }
        const Deadline deadline;
        const auto directory = config.sdk.lexically_normal();
        // cuDNN resolves some graph/engine components after LoadLibraryEx's
        // temporary dependency search has ended. Retain the complete SDK-local
        // set for the same lifetime as the CUDA modules and their operations.
        engine::detail::SdkRuntimeDependencies runtime_dependencies;
        std::string runtime_error;
        if (!engine::detail::load_sdk_runtime_dependencies(directory / "ggml-cuda.dll",
                runtime_dependencies, &runtime_error))
            throw std::runtime_error(runtime_error);
        const Module base(directory / "ggml-base.dll");
        const Module core(directory / "ggml.dll");
        const Module cuda(directory / "ggml-cuda.dll");
        const Api api(base.get(), core.get());
        api.ggml_log_set([](ggml_log_level, const char *text, void *) { std::fputs(text, stderr); }, nullptr);
        const Registration registration{api, api.ggml_backend_load(utf8(directory / "ggml-cuda.dll").c_str())};
        if (!registration.value || api.ggml_backend_reg_dev_count(registration.value) == 0)
            throw std::runtime_error("matching CUDA backend has no available device");
        const auto device = api.ggml_backend_reg_dev_get(registration.value, 0);
        std::unique_ptr<ggml_backend, decltype(api.ggml_backend_free)> backend(
            api.ggml_backend_dev_init(device, nullptr), api.ggml_backend_free);
        if (!backend) throw std::runtime_error("CUDA backend initialization failed");
        std::string cases = convolution(api, backend.get(), false);
        if (config.bias) cases += ',' + convolution(api, backend.get(), true);
        const auto cudnn = module_path(L"cudnn64_9.dll");
        if (cudnn.empty()) throw std::runtime_error("convolution completed without the required cuDNN 9 runtime loaded");
        const auto cudnn_modules = loaded_cudnn_modules(directory);
        return emit("{\"schema\":1,\"status\":\"measured\",\"operation\":\"CONV_2D_DIRECT_CUDNN\",\"dtype\":\"f16\","
            "\"backend\":" + quote(api.ggml_backend_dev_name(device)) + ",\"sdk_dir\":" + quote(utf8(directory)) +
            ",\"cuda_module\":" + quote(module_path(L"ggml-cuda.dll")) + ",\"cudnn_module\":" + quote(cudnn) +
            ",\"loaded_cudnn_modules\":" + cudnn_modules +
            ",\"shape\":{\"W\":8,\"H\":8,\"IC\":8,\"OC\":8,\"N\":1,\"KW\":1,\"KH\":1},\"cases\":[" + cases + "]}\n") ? 0 : 1;
    }
    catch (const std::exception &exception)
    {
        emit("{\"schema\":1,\"status\":\"failed\",\"error\":" + quote(exception.what()) + "}\n");
        return 1;
    }
}
}

int wmain(int argc, wchar_t **argv) { return hmi::diagnostics::run(argc, argv); }
