#include "engine/framework/audio/utilities/gtcrn.h"
#include "engine/framework/audio/utilities/hgtcrn.h"

#include <ggml-backend.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kBins = 257;
constexpr std::size_t kFeatures = 6;
constexpr std::size_t kMaximumFrames = 1877;
static_assert(sizeof(float) == 4, "Reference fixtures require 32-bit floats");

double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string utf8(const std::filesystem::path & path) {
    const auto encoded = path.u8string();
    return std::string(reinterpret_cast<const char *>(encoded.data()), encoded.size());
}

std::filesystem::path executable_path(const char * argument) {
#if defined(_WIN32)
    (void)argument;
    std::vector<wchar_t> path(512);
    while (path.size() <= 32768) {
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) throw std::runtime_error("Could not locate the diagnostic executable");
        if (length < path.size()) return std::filesystem::path(std::wstring(path.data(), length));
        path.resize(path.size() * 2);
    }
    throw std::runtime_error("Diagnostic executable path is too long");
#elif defined(__APPLE__)
    (void)argument;
    std::uint32_t length = 0;
    (void)_NSGetExecutablePath(nullptr, &length);
    std::vector<char> path(length);
    if (_NSGetExecutablePath(path.data(), &length) != 0) throw std::runtime_error("Could not locate the diagnostic executable");
    return std::filesystem::weakly_canonical(std::filesystem::u8path(path.data()));
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return path;
    return std::filesystem::absolute(std::filesystem::u8path(argument));
#endif
}

std::size_t frame_count(const char * argument) {
    const std::string token(argument);
    std::size_t frames = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), frames);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
        frames == 0 || frames > kMaximumFrames) {
        throw std::invalid_argument("frames must be an integer between 1 and 1877");
    }
    return frames;
}

std::filesystem::path input_path(const char * argument) {
    const auto path = std::filesystem::weakly_canonical(std::filesystem::u8path(argument));
    if (!std::filesystem::is_regular_file(path)) throw std::invalid_argument("An input path is not a regular file: " + utf8(path));
    return path;
}

std::vector<float> read_features(const std::filesystem::path & path, std::size_t frames) {
    const auto count = frames * kFeatures * kBins;
    const auto bytes = count * sizeof(float);
    if (std::filesystem::file_size(path) != bytes) {
        throw std::invalid_argument("Feature file must contain exactly frames * 6 * 257 float32 values");
    }
    std::vector<float> features(count);
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char *>(features.data()), static_cast<std::streamsize>(bytes))) {
        throw std::runtime_error("Could not read the feature fixture");
    }
    for (const float value : features) {
        if (!std::isfinite(value)) throw std::invalid_argument("Feature fixture contains non-finite values");
    }
    return features;
}

double load_cpu_backends(const char * executable) {
#ifdef _WIN32
    if (_putenv_s("OMP_NUM_THREADS", "1") != 0 || _putenv_s("OMP_DYNAMIC", "FALSE") != 0) {
        throw std::runtime_error("Could not configure single-threaded OpenMP diagnostics");
    }
#else
    if (setenv("OMP_NUM_THREADS", "1", 1) != 0 || setenv("OMP_DYNAMIC", "FALSE", 1) != 0) {
        throw std::runtime_error("Could not configure single-threaded OpenMP diagnostics");
    }
#endif
#ifdef _OPENMP
    omp_set_dynamic(0);
    omp_set_num_threads(1);
#endif
    const auto start = Clock::now();
    const auto directory = utf8(executable_path(executable).parent_path());
    ggml_backend_load_all_from_path(directory.c_str());
    return milliseconds(start);
}

void validate_output(const std::filesystem::path & path, const std::filesystem::path & weights,
                     const std::filesystem::path & input) {
    const bool aliases_input = std::filesystem::exists(path) &&
        (std::filesystem::equivalent(path, weights) || std::filesystem::equivalent(path, input));
    if (aliases_input || path == weights || path == input ||
        !std::filesystem::is_directory(path.parent_path()) || std::filesystem::is_directory(path)) {
        throw std::invalid_argument("Output must be a separate file in an existing directory");
    }
}

void write_binary(const std::filesystem::path & path, const void * data, std::size_t bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(static_cast<const char *>(data), static_cast<std::streamsize>(bytes));
    file.close();
    if (!file) throw std::runtime_error("Could not write diagnostic output: " + utf8(path));
}

struct PipelineFiles {
    std::filesystem::path prefix;

    std::filesystem::path path(const char * stage) const {
        auto output = prefix;
        output += "." + std::string(stage) + ".f32";
        return output;
    }

    static void observe(void * context, const char * stage, const std::complex<float> * data,
                        std::size_t channels, std::size_t frequency, std::size_t frames) {
        static_assert(sizeof(std::complex<float>) == 2 * sizeof(float), "Complex fixture layout requires two floats");
        if (context == nullptr || data == nullptr || channels == 0 || channels > 2 ||
            frequency != kBins || frames == 0 || frames > kMaximumFrames) {
            throw std::runtime_error("H-GTCRN diagnostic stage has an invalid shape");
        }
        const auto count = channels * frequency * frames;
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(data[i].real()) || !std::isfinite(data[i].imag())) {
                throw std::runtime_error("H-GTCRN diagnostic stage is not finite");
            }
        }
        const auto & files = *static_cast<PipelineFiles *>(context);
        const auto output = files.path(stage);
        write_binary(output, data, count * sizeof(std::complex<float>));
        std::printf("hgtcrn_pipeline_stage stage=%s channels=%zu frequency=%zu frames=%zu "
                    "layout=channel_frequency_time_complex output=\"%s\"\n",
                    stage, channels, frequency, frames, utf8(output).c_str());
    }
};

int run_pipeline(char ** argv) {
    const auto weights = input_path(argv[2]);
    const auto input = input_path(argv[3]);
    const auto bytes = std::filesystem::file_size(input);
    if (bytes % (2 * sizeof(float)) != 0 || bytes < 4608 * 2 * sizeof(float) || bytes > 480000 * 2 * sizeof(float)) {
        throw std::invalid_argument("Stereo fixture must contain 4608 to 480000 interleaved dual-channel frames");
    }
    const auto frames = static_cast<std::size_t>(bytes / (2 * sizeof(float)));
    PipelineFiles files{std::filesystem::weakly_canonical(std::filesystem::absolute(std::filesystem::u8path(argv[4])))};
    const std::array<const char *, 5> stages{"stft", "wpe", "iva", "network", "output"};
    for (const auto * stage : stages) validate_output(files.path(stage), weights, input);
    std::vector<float> samples(2 * frames);
    std::ifstream stream(input, std::ios::binary);
    if (!stream.read(reinterpret_cast<char *>(samples.data()), static_cast<std::streamsize>(bytes))) {
        throw std::runtime_error("Could not read the stereo fixture");
    }
    for (const auto value : samples) {
        if (!std::isfinite(value)) throw std::invalid_argument("Stereo fixture contains non-finite values");
    }
    const double backend_ms = load_cpu_backends(argv[0]);
    engine::core::BackendConfig backend;
    backend.type = engine::core::BackendType::Cpu;
    backend.threads = 1;
    const auto load_start = Clock::now();
    engine::audio::HgtcrnModel model(weights, backend);
    const double load_ms = milliseconds(load_start);
    const engine::audio::HgtcrnStageObserver observer{&files, &PipelineFiles::observe};
    const auto output = model.run(samples.data(), frames, 1, &observer);
    write_binary(files.path("output"), output.data(), output.size() * sizeof(float));
    const auto measured = model.statistics();
    std::printf("hgtcrn_pipeline_probe input_frames=%zu output_frames=%zu stft_frames=%llu right_padding_frames=%u "
                "iva_speech_channel=%u cpu_threads=1 backend_load_ms=%.3f model_graph_load_ms=%.3f "
                "stft_ms=%.3f wpe_ms=%.3f iva_ms=%.3f network_ms=%.3f istft_ms=%.3f total_including_observer_io_ms=%.3f\n",
                frames, output.size(), static_cast<unsigned long long>(measured.stft_frames), measured.right_padding_frames,
                measured.iva_speech_channel, backend_ms, load_ms, measured.stft_ms, measured.wpe_ms,
                measured.iva_ms, measured.network_ms, measured.istft_ms, measured.total_ms);
    return 0;
}

}  // namespace

int main(int argc, char ** argv) {
    try {
        if (argc != 5) {
            std::fprintf(stderr, "usage: hgtcrn_reference_probe weights.safetensors features.f32 frames output_complex.f32\n"
                                 "  input: float32 [frames, 6, 257], output: float32 [frames, 257, 2]\n"
                                 "  or: hgtcrn_reference_probe --pipeline weights.safetensors input_stereo.f32 output_prefix\n");
            return 2;
        }
        if (std::string(argv[1]) == "--pipeline") return run_pipeline(argv);
        const auto weights = input_path(argv[1]);
        const auto features_path = input_path(argv[2]);
        const auto frames = frame_count(argv[3]);
        const auto output_path = std::filesystem::weakly_canonical(std::filesystem::absolute(std::filesystem::u8path(argv[4])));
        validate_output(output_path, weights, features_path);
        const auto features = read_features(features_path, frames);
        const double backend_ms = load_cpu_backends(argv[0]);
        engine::core::BackendConfig backend;
        backend.type = engine::core::BackendType::Cpu;
        backend.threads = 1;
        const auto load_start = Clock::now();
        const auto model = engine::audio::GTCRNModel::load_hgtcrn_from_safetensors(weights, backend);
        auto session = model.create_streaming_session();
        const double load_ms = milliseconds(load_start);
        std::vector<float> output(frames * kBins * 2);
        std::array<float, kBins * 2> reference{};
        const auto network_start = Clock::now();
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const float * input = features.data() + frame * kFeatures * kBins;
            for (std::size_t frequency = 0; frequency < kBins; ++frequency) {
                reference[2 * frequency] = input[frequency];
                reference[2 * frequency + 1] = input[kBins + frequency];
            }
            session->process_hgtcrn_frame(input, reference.data(), output.data() + frame * kBins * 2);
        }
        const double network_ms = milliseconds(network_start);
        for (const float value : output) {
            if (!std::isfinite(value)) throw std::runtime_error("H-GTCRN graph returned non-finite values");
        }
        write_binary(output_path, output.data(), output.size() * sizeof(float));
        std::printf("hgtcrn_graph_probe frames=%zu input_floats=%zu output_floats=%zu cpu_threads=1 "
                    "backend_load_ms=%.3f model_graph_load_ms=%.3f network_ms=%.3f output=\"%s\"\n",
                    frames, features.size(), output.size(), backend_ms, load_ms, network_ms, utf8(output_path).c_str());
        return 0;
    } catch (const std::exception & error) {
        std::fprintf(stderr, "hgtcrn_graph_probe error: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "hgtcrn_graph_probe error: unknown exception\n");
        return 1;
    }
}
