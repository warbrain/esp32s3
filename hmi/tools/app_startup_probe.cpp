#include "capture/audio_capture.hpp"
#include "engine/asr_session.hpp"
#include "engine/native_client.hpp"
#include "engine/tts_session.hpp"
#include "input/hotkey.hpp"
#include "base/performance.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <cstdint>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

std::string utf8(const std::filesystem::path &path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

struct Options
{
    std::filesystem::path models;
    std::string backend = "auto";
    int threads = 4;
    int timeout = 180;
    int tts_idle_check = 0;
    bool capture = false;
    bool hotkeys = false;
    bool sdk_only = false;
    bool help = false;
};

int integer(const std::wstring &text, int maximum)
{
    std::size_t consumed = 0;
    const int value = std::stoi(text, &consumed);
    if (consumed != text.size() || value < 1 || value > maximum)
        throw std::runtime_error("numeric option outside allowed range");
    return value;
}

Options options(int argc, wchar_t **argv)
{
    Options out;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring key = argv[index];
        if (key == L"--help") out.help = true;
        else if (key == L"--check-capture") out.capture = true;
        else if (key == L"--check-hotkeys") out.hotkeys = true;
        else if (key == L"--sdk-only") out.sdk_only = true;
        else
        {
            if (++index == argc) throw std::runtime_error("missing option value");
            const std::wstring value = argv[index];
            if (key == L"--model-dir") out.models = value;
            else if (key == L"--threads") out.threads = integer(value, 256);
            else if (key == L"--timeout") out.timeout = integer(value, 600);
            else if (key == L"--tts-idle-check") out.tts_idle_check = integer(value, 30);
            else if (key == L"--backend")
            {
                if (value == L"auto") out.backend = "auto";
                else if (value == L"cpu") out.backend = "cpu";
                else if (value == L"cuda") out.backend = "cuda";
                else if (value == L"vulkan") out.backend = "vulkan";
                else throw std::runtime_error("backend must be auto, cpu, cuda, or vulkan");
            }
            else throw std::runtime_error("unknown option");
        }
    }
    return out;
}

// Only this diagnostic process is terminated if a driver or model call hangs.
// Native model APIs do not provide a cancellable initialization deadline.
class Deadline
{
public:
    explicit Deadline(int seconds) : worker_([this, seconds] {
        std::unique_lock lock(mutex_);
        if (condition_.wait_for(lock, std::chrono::seconds(seconds), [this] { return finished_; })) return;
        std::fprintf(stderr, "STARTUP FAIL stage=deadline timeout_seconds=%d\n", seconds);
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 124);
    }) {}

    ~Deadline()
    {
        { std::lock_guard lock(mutex_); finished_ = true; }
        condition_.notify_one();
        worker_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool finished_ = false;
    std::thread worker_;
};

void report(const char *stage, const char *status, Clock::time_point start, const std::string &detail)
{
    const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::printf("STARTUP %s stage=%s ms=%.3f detail=%s\n", status, stage, milliseconds, detail.c_str());
    std::fflush(stdout);
}

void memory(const char *stage)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                static_cast<DWORD>(sizeof(counters))))
        throw std::runtime_error("cannot sample this diagnostic process's memory");
    std::printf("STARTUP memory stage=%s working_mib=%.3f peak_mib=%.3f private_mib=%.3f\n", stage,
        static_cast<double>(counters.WorkingSetSize) / 1048576.0,
        static_cast<double>(counters.PeakWorkingSetSize) / 1048576.0,
        static_cast<double>(counters.PrivateUsage) / 1048576.0);
    std::fflush(stdout);
}

// Diagnostic conversion only; production capture uses WASAPI AUTOCONVERTPCM.
std::vector<float> mono_16k(const hmi::audio::AudioBuffer &audio)
{
    if (audio.channels < 1 || audio.sample_rate < 8000 || audio.samples.empty() ||
        audio.samples.size() % static_cast<std::size_t>(audio.channels) != 0)
        throw std::runtime_error("cannot resample invalid TTS output");
    const auto channels = static_cast<std::size_t>(audio.channels);
    const auto input_frames = audio.samples.size() / channels;
    const auto output_frames = static_cast<std::size_t>(std::llround(
        static_cast<double>(input_frames) * 16000.0 / static_cast<double>(audio.sample_rate)));
    if (output_frames == 0 || output_frames >= static_cast<std::size_t>(16000) * 30)
        throw std::runtime_error("idle validation requires nonempty PCM shorter than 30 seconds");
    std::vector<float> output(output_frames);
    for (std::size_t frame = 0; frame < output_frames; ++frame)
    {
        const double position = static_cast<double>(frame) * static_cast<double>(audio.sample_rate) / 16000.0;
        const auto left = std::min(static_cast<std::size_t>(position), input_frames - 1);
        const auto right = std::min(left + 1, input_frames - 1);
        const auto fraction = static_cast<float>(position - static_cast<double>(left));
        float sum = 0.0f;
        for (std::size_t channel = 0; channel < channels; ++channel)
            sum += audio.samples[left * channels + channel] * (1.0f - fraction) +
                   audio.samples[right * channels + channel] * fraction;
        output[frame] = sum / static_cast<float>(channels);
    }
    return output;
}

std::uint64_t text_fingerprint(const std::string &text)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const char value : text)
    {
        hash ^= static_cast<unsigned char>(value);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool recognize(hmi::engine::AsrSession &asr, const std::vector<float> &pcm,
               hmi::engine::Transcript &out, std::string *error)
{
    if (!asr.ready() || !asr.begin(error)) return false;
    std::vector<hmi::engine::Transcript> partial;
    const auto chunk = std::max<std::size_t>(1, asr.preferred_chunk_samples());
    for (std::size_t cursor = 0; cursor < pcm.size(); cursor += chunk)
        if (!asr.push(pcm.data() + cursor, std::min(chunk, pcm.size() - cursor), partial, error)) return false;
    return asr.finish(out, error);
}

bool idle_validation(hmi::engine::AsrSession &asr, hmi::engine::TtsSession &tts,
                     const hmi::audio::AudioBuffer &generated, int seconds,
                     hmi::base::PerformanceSampler &performance)
{
    const auto resources = [&performance](const char *stage) {
        memory(stage);
        const auto sample = performance.snapshot();
        std::printf("STARTUP gpu_memory stage=%s available=%d dedicated_mib=%.3f scope=process\n",
            stage, sample.gpu_memory_available ? 1 : 0, sample.gpu_memory_mib);
    };
    const auto pcm = mono_16k(generated);
    resources("dual_models_before_idle");
    auto started = Clock::now();
    hmi::engine::Transcript before;
    std::string error;
    if (!recognize(asr, pcm, before, &error) || !before.final || before.text.empty())
    {
        report("asr_before_idle", "FAIL", started, error.empty() ? "no nonempty final transcript" : error);
        return false;
    }
    report("asr_before_idle", "PASS", started, "same generated PCM; frames=" + std::to_string(pcm.size()) +
        " text_bytes=" + std::to_string(before.text.size()));
    resources("dual_models_after_initial_recognition");
    started = Clock::now();
    const auto expiry = started + std::chrono::seconds(seconds + 5);
    auto state = tts.statistics();
    while ((state.model_ready || state.idle_unloads == 0) && Clock::now() < expiry)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        state = tts.statistics();
    }
    if (state.model_ready || state.idle_unloads == 0 || !asr.ready())
    {
        report("tts_idle_release", "FAIL", started, "TTS did not unload or resident ASR became unavailable");
        return false;
    }
    report("tts_idle_release", "PASS", started, "model_ready=0 idle_unloads=" + std::to_string(state.idle_unloads) +
        " cache_bytes=" + std::to_string(state.cache_bytes) + " idle_seconds=" + std::to_string(seconds));
    // Allow the one-second process GPU counter to observe released buffers.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    resources("asr_resident_after_tts_idle");
    started = Clock::now();
    hmi::engine::Transcript after;
    if (!recognize(asr, pcm, after, &error) || !after.final || after.text.empty() || after.text != before.text)
    {
        report("asr_after_idle", "FAIL", started, error.empty() ? "final transcript changed after TTS release" : error);
        return false;
    }
    report("asr_after_idle", "PASS", started, "same PCM and identical nonempty final text; correctness against a reference not assessed");
    std::printf("STARTUP asr_idle_consistency same_pcm=1 same_text=1 frames=%zu text_bytes=%zu fnv1a64=%016llx\n",
        pcm.size(), before.text.size(), static_cast<unsigned long long>(text_fingerprint(before.text)));
    resources("asr_after_idle_recognition");
    return true;
}
}

int wmain(int argc, wchar_t **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    SetConsoleOutputCP(CP_UTF8);
    try
    {
        const Options config = options(argc, argv);
        if (config.help)
        {
            std::printf("usage: hmi_app_startup_probe [--model-dir PATH] [--backend auto|cpu|cuda|vulkan] "
                "[--threads 1..256] [--timeout 1..600] [--check-hotkeys] [--check-capture]\n"
                "Loads ASR, retains it while TTS loads and synthesizes one phrase, then releases both.\n"
                "No playback, input injection or actions. Microphone and global hotkeys are skipped unless explicitly requested.\n"
                "--tts-idle-check 1..30 enables ASR consistency and memory checks before/after TTS idle release; default skips recognition.\n"
                "--sdk-only reports SDK initialization stages without loading models, microphone, or hotkeys.\n"
                "--check-capture briefly opens the microphone and discards samples; --check-hotkeys checks registration only.\n");
            return 0;
        }
        const Deadline deadline(config.timeout);
        if (config.tts_idle_check != 0)
        {
            const auto seconds = std::to_wstring(config.tts_idle_check);
            if (!SetEnvironmentVariableW(L"HMI_TTS_IDLE_SECONDS", seconds.c_str()))
                throw std::runtime_error("cannot set process-local TTS idle policy");
        }
        const auto total = Clock::now();
        hmi::base::PerformanceSampler performance;
        if (config.tts_idle_check != 0) performance.start();
        std::printf("STARTUP backend=%s threads=%d microphone=%s hotkeys=%s\n", config.backend.c_str(),
            config.threads, config.capture ? "requested" : "skipped", config.hotkeys ? "requested" : "skipped");
        std::fflush(stdout);
        hmi::engine::Client client;
        std::string error;
        auto start = Clock::now();
        if (!client.open(&error, [](const hmi::engine::SdkInitializationStage &stage) {
            std::printf("STARTUP sdk_stage=%s phase=%s ms=%.3f success=%d detail=%s\n",
                stage.name.c_str(), stage.completed ? "completed" : "started", stage.elapsed_ms,
                stage.success ? 1 : 0, stage.detail.c_str());
            std::fflush(stdout);
        })) { report("sdk", "FAIL", start, error); return 3; }
        const auto version = client.version();
        report("sdk", "PASS", start, "abi=" + std::to_string(version.abi) + " build=" + version.build);

        if (config.sdk_only)
        {
            client.close();
            report("shutdown", "PASS", total, "SDK-only stages completed; no models, capture, playback or input");
            return 0;
        }

        hmi::engine::AsrConfig asr_config;
        hmi::engine::TtsConfig tts_config;
        asr_config.backend = tts_config.backend = config.backend;
        asr_config.threads = tts_config.threads = config.threads;
        if (config.tts_idle_check != 0) asr_config.preview_interval_seconds = 30.0;
        if (!config.models.empty())
        {
            asr_config.model_path = utf8(config.models / "asr.gguf");
            tts_config.model_path = utf8(config.models / "tts.gguf");
        }
        hmi::engine::AsrSession asr;
        start = Clock::now();
        if (!asr.open(client, asr_config, &error)) { report("asr_model", "FAIL", start, error); return 4; }
        report("asr_model", "PASS", start, asr_config.model_path);

        hmi::engine::TtsSession tts;
        hmi::audio::AudioBuffer generated;
        start = Clock::now();
        if (!tts.open(client, tts_config, &error) ||
            !tts.synthesize(config.tts_idle_check != 0 ? "你好，语音操控已就绪。" : "你好", generated, &error))
        { report("tts_warmup", "FAIL", start, error); return 5; }
        if (generated.sample_rate < 8000 || generated.channels < 1 || generated.samples.empty() ||
            !std::all_of(generated.samples.begin(), generated.samples.end(), [](float value) { return std::isfinite(value); }))
        { report("tts_warmup", "FAIL", start, "SDK did not return valid owned PCM"); return 5; }
        report("tts_warmup", "PASS", start, tts_config.model_path + " frames=" +
            std::to_string(generated.samples.size() / static_cast<std::size_t>(generated.channels)));
        if (config.tts_idle_check != 0 && !idle_validation(asr, tts, generated, config.tts_idle_check, performance)) return 8;

        if (config.capture)
        {
            hmi::capture::AudioCapture capture;
            start = Clock::now();
            if (!capture.start([](const float *, std::size_t, std::int64_t) {}, &error))
            { report("microphone", "FAIL", start, error); return 6; }
            capture.stop();
            report("microphone", "PASS", start, "WASAPI initialized; samples discarded");
        }
        if (config.hotkeys)
        {
            hmi::input::Hotkeys hotkeys;
            start = Clock::now();
            if (!hotkeys.start({}, [](hmi::input::HotkeyEvent) {}, &error))
            { report("hotkeys", "FAIL", start, error); return 7; }
            const std::string bindings = "hold=" + hotkeys.bindings().hold + " toggle=" + hotkeys.bindings().toggle;
            hotkeys.stop();
            report("hotkeys", error.empty() ? "PASS" : "WARN", start,
                error.empty() ? bindings + "; registration only; no keys injected" : error);
        }
        report(config.tts_idle_check != 0 ? "idle_validation" : "models_resident", "PASS", total,
            config.tts_idle_check != 0 ? "TTS idle release and resident ASR recognition consistency verified" :
            "ASR session and TTS warmup completed together; recognition not exercised");
        tts.stop();
        asr.close();
        client.close();
        report("shutdown", "PASS", total, "sessions and SDK released");
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::fprintf(stderr, "STARTUP FAIL exception=%s\n", exception.what());
        return 2;
    }
}
