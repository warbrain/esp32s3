#include "engine/audio_preprocessor.hpp"
#include "engine/hgtcrn_session.hpp"
#include "engine/native_client.hpp"
#include "base/project_paths.hpp"
#include "util/json_writer.hpp"

#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
using Value = hmi::util::json::Value;

struct Options
{
    std::filesystem::path input, output, report, hgtcrn_model, vad_model;
    int channels = 2;
    std::size_t chunk = 4000;
    int iterations = 1;
};

std::filesystem::path project_path(const std::filesystem::path &path, bool build_only = false)
{
    std::filesystem::path selected;
    std::string error;
    if (!hmi::base::resolve_project_path(path, hmi::base::process_directory(), selected, &error))
        throw std::runtime_error(error);
    if (build_only)
    {
        const auto build = std::filesystem::weakly_canonical(
            hmi::base::project_root(hmi::base::process_directory()) / L"build");
        auto part = selected.begin();
        for (const auto &root_part : build)
        {
            if (part == selected.end() || CompareStringOrdinal(root_part.c_str(), -1, part->c_str(), -1, TRUE) != CSTR_EQUAL)
                throw std::runtime_error("probe outputs must stay in project/build");
            ++part;
        }
    }
    return selected;
}

Options options(int argc, wchar_t **argv)
{
    Options selected;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring key = argv[index];
        if (++index == argc) throw std::runtime_error("missing option value");
        const std::wstring value = argv[index];
        if (key == L"--input") selected.input = value;
        else if (key == L"--output") selected.output = value;
        else if (key == L"--report") selected.report = value;
        else if (key == L"--hgtcrn-model") selected.hgtcrn_model = value;
        else if (key == L"--vad-model") selected.vad_model = value;
        else if (key == L"--channels" && (value == L"1" || value == L"2")) selected.channels = value == L"1" ? 1 : 2;
        else if (key == L"--chunk")
        {
            std::size_t consumed = 0;
            const auto count = std::stoul(value, &consumed);
            if (consumed != value.size() || count == 0 || count > 480000) throw std::runtime_error("chunk must be 1..480000");
            selected.chunk = count;
        }
        else if (key == L"--iterations")
        {
            std::size_t consumed = 0;
            selected.iterations = std::stoi(value, &consumed);
            if (consumed != value.size() || selected.iterations < 1 || selected.iterations > 20)
                throw std::runtime_error("iterations must be 1..20");
        }
        else throw std::runtime_error("invalid probe option");
    }
    if (selected.input.empty() || selected.output.empty() || selected.report.empty() ||
        (!selected.hgtcrn_model.empty() && selected.channels != 2))
        throw std::runtime_error("input/output/report required; H-GTCRN requires channels=2");
    selected.input = project_path(selected.input);
    selected.output = project_path(selected.output, true);
    selected.report = project_path(selected.report, true);
    if (!selected.hgtcrn_model.empty()) selected.hgtcrn_model = project_path(selected.hgtcrn_model);
    if (!selected.vad_model.empty()) selected.vad_model = project_path(selected.vad_model);
    return selected;
}

double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double process_cpu_ms()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw std::runtime_error("cannot read process CPU time");
    const auto ticks = [](FILETIME time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
}

Value memory_json()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
        throw std::runtime_error("cannot read process memory");
    return Value(Value::Object{
        {"working_set_mib", Value(static_cast<double>(counters.WorkingSetSize) / 1048576.0)},
        {"private_mib", Value(static_cast<double>(counters.PrivateUsage) / 1048576.0)},
        {"peak_working_set_mib", Value(static_cast<double>(counters.PeakWorkingSetSize) / 1048576.0)}});
}

std::vector<float> read_pcm(const Options &selected)
{
    std::ifstream file(selected.input, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open input PCM");
    const auto length = file.tellg();
    const auto block = static_cast<std::streamoff>(sizeof(float) * selected.channels);
    if (length <= 0 || length % block != 0 || length / block > 480000)
        throw std::runtime_error("input must contain 1..480000 complete float32 frames at 16000 Hz");
    std::vector<float> samples(static_cast<std::size_t>(length) / sizeof(float));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(samples.data()), static_cast<std::streamsize>(length)))
        throw std::runtime_error("cannot read input PCM");
    if (!std::all_of(samples.begin(), samples.end(), [](float value) { return std::isfinite(value); }))
        throw std::runtime_error("input PCM must be finite");
    return samples;
}

Value frontend_json(const hmi::engine::AudioPreprocessorStatistics &s)
{
    return Value(Value::Object{
        {"input_frames", Value(static_cast<double>(s.input_frames))},
        {"output_frames", Value(static_cast<double>(s.output_frames))},
        {"pending_frames", Value(static_cast<double>(s.pending_frames))},
        {"vad_chunks", Value(static_cast<double>(s.vad_chunks))},
        {"voiced_chunks", Value(static_cast<double>(s.voiced_chunks))},
        {"uncertain_chunks", Value(static_cast<double>(s.uncertain_chunks))},
        {"speech_seen", Value(s.speech_seen)},
        {"max_vad_probability", Value(static_cast<double>(s.max_vad_probability))},
        {"vad_ms", Value(s.vad_ms)}, {"wall_ms", Value(s.wall_ms)}});
}

Value enhancement_json(const hmi::engine::HgtcrnStatistics &s)
{
    return Value(Value::Object{
        {"succeeded", Value(s.succeeded)}, {"channels", Value(static_cast<double>(s.channels))},
        {"input_frames", Value(static_cast<double>(s.input_frames))},
        {"output_frames", Value(static_cast<double>(s.output_frames))},
        {"stft_frames", Value(static_cast<double>(s.stft_frames))},
        {"wpe_taps", Value(static_cast<double>(s.wpe_taps))},
        {"iva_iterations", Value(static_cast<double>(s.iva_iterations))},
        {"iva_speech_channel", Value(static_cast<double>(s.iva_speech_channel))},
        {"right_padding_frames", Value(static_cast<double>(s.right_padding_frames))},
        {"channel_correlation", Value(s.channel_correlation)}, {"channel_rms_ratio", Value(s.channel_rms_ratio)},
        {"stft_ms", Value(s.stft_ms)}, {"wpe_ms", Value(s.wpe_ms)}, {"iva_ms", Value(s.iva_ms)},
        {"network_ms", Value(s.network_ms)}, {"istft_ms", Value(s.istft_ms)}, {"total_ms", Value(s.total_ms)}});
}
}

int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc == 1 || (argc == 2 && std::wstring(argv[1]) == L"--help"))
        {
            std::puts("Usage: hmi_audio_frontend_probe --input PCM.f32 --channels 1|2 --output BUILD.f32 --report BUILD.json\n"
                "  [--hgtcrn-model MODEL.safetensors] [--vad-model MODEL.safetensors] [--chunk 4000] [--iterations 1]\n"
                "Input is 16000 Hz interleaved float32. Outputs stay in project/build. No capture, playback or text input.");
            return 0;
        }
        const auto selected = options(argc, argv);
        const auto input = read_pcm(selected);
        const auto frames = input.size() / static_cast<std::size_t>(selected.channels);
        hmi::engine::Client client;
        std::string error;
        const auto sdk_started = Clock::now();
        if (!client.open(&error)) throw std::runtime_error(error);
        const auto sdk_ms = elapsed(sdk_started);
        hmi::engine::HgtcrnSession enhancer;
        const auto load_started = Clock::now();
        if (!selected.hgtcrn_model.empty())
        {
            hmi::engine::HgtcrnConfig config;
            config.model = selected.hgtcrn_model;
            if (!enhancer.open(client, config, &error)) throw std::runtime_error(error);
        }
        const auto model_ms = elapsed(load_started);
        hmi::engine::AudioPreprocessor frontend;
        hmi::engine::AudioPreprocessorConfig config;
        config.denoise_enabled = false;
        config.vad_enabled = !selected.vad_model.empty();
        config.vad_model = selected.vad_model;
        if (!frontend.open(client, config, &error)) throw std::runtime_error(error);
        Value::Array reports;
        std::vector<float> output;
        for (int iteration = 1; iteration <= selected.iterations; ++iteration)
        {
            const auto started = Clock::now();
            const auto cpu_started = process_cpu_ms();
            std::vector<float> mono;
            if (enhancer.ready())
            {
                if (!enhancer.run(input.data(), frames, static_cast<std::uint64_t>(iteration), mono, &error))
                    throw std::runtime_error(error);
            }
            else
            {
                mono.resize(frames);
                for (std::size_t i = 0; i < frames; ++i)
                    mono[i] = selected.channels == 1 ? input[i] : 0.5f * (input[2 * i] + input[2 * i + 1]);
            }
            if (!frontend.begin(static_cast<std::uint64_t>(iteration), &error)) throw std::runtime_error(error);
            output.clear();
            output.reserve(frames);
            std::vector<float> chunk;
            for (std::size_t offset = 0; offset < frames; offset += selected.chunk)
            {
                const auto count = std::min(selected.chunk, frames - offset);
                if (!frontend.push(mono.data() + offset, count, chunk, &error)) throw std::runtime_error(error);
                output.insert(output.end(), chunk.begin(), chunk.end());
            }
            if (!frontend.finish(chunk, &error)) throw std::runtime_error(error);
            output.insert(output.end(), chunk.begin(), chunk.end());
            if (output.size() != frames || !std::all_of(output.begin(), output.end(), [](float value) { return std::isfinite(value); }))
                throw std::runtime_error("processed PCM duration or values invalid");
            double square = 0.0;
            for (const auto sample : output) square += static_cast<double>(sample) * sample;
            reports.emplace_back(Value::Object{
                {"iteration", Value(static_cast<double>(iteration))}, {"sdk_open_ms", Value(sdk_ms)},
                {"hgtcrn_load_ms", Value(model_ms)}, {"audio_seconds", Value(static_cast<double>(frames) / 16000.0)},
                {"total_wall_ms", Value(elapsed(started))}, {"rms", Value(std::sqrt(square / static_cast<double>(frames)))},
                {"process_cpu_ms", Value(process_cpu_ms() - cpu_started)}, {"memory", memory_json()},
                {"frontend", frontend_json(frontend.statistics())}, {"hgtcrn", enhancement_json(enhancer.statistics())}});
        }
        std::filesystem::create_directories(selected.output.parent_path());
        std::filesystem::create_directories(selected.report.parent_path());
        std::ofstream pcm(selected.output, std::ios::binary | std::ios::trunc);
        if (!pcm.write(reinterpret_cast<const char *>(output.data()), static_cast<std::streamsize>(output.size() * sizeof(float))))
            throw std::runtime_error("cannot write processed PCM");
        const auto report = hmi::util::json::stringify(Value(std::move(reports)));
        std::ofstream json(selected.report, std::ios::binary | std::ios::trunc);
        if (!(json << report << '\n')) throw std::runtime_error("cannot write statistics report");
        std::puts(report.c_str());
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "Audio frontend probe failed: %s\n", error.what());
        return 1;
    }
}
