#include "audio/audio_render.hpp"
#include "engine/asr_session.hpp"
#include "engine/native_client.hpp"
#include "engine/tts_session.hpp"
#include "engine/sdk_threads.hpp"
#include "base/project_paths.hpp"
#include "engine/tts_pipeline.hpp"
#include "probe_resample.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <array>
#include <condition_variable>
#include <mutex>

namespace
{
using Clock = std::chrono::steady_clock;
struct Options
{
    std::string backend = "auto";
    int threads = 4;
    int iterations = 3;
    bool iterations_set = false;
    bool stream = false;
    bool realtime = false;
    bool prepare_tts = false;
    bool readback_probe = false;
    bool final_only = false;
    bool help = false;
    bool tts_segments = false;
    bool tts_muted_playback = false;
    bool tts_fast_sink = false;
    bool tts_cold = false;
    int tts_idle_check = 0;
    std::wstring tts_cache;
    double preview_interval_seconds = 1.0;
    double minimum_audio_seconds = -1.0;
    std::string asr_text = "打开计算器";
    std::string expect_text = "计算器";
    std::wstring tts_perf_mode;
    std::string tts_text = "你好，这是分段语音合成测试。第一段生成后就可以开始播放，后面的内容会继续生成，不必等待整段文字全部合成。我们现在检查首段等待时间、后续分段速度和缓冲上限。";
    std::filesystem::path pcm;
    std::filesystem::path save_pcm;
    std::filesystem::path save_tts_pcm;
    int pcm_rate = 16000;
    hmi::probe::Resampler resampler = hmi::probe::Resampler::Linear;
};

std::string option_text(const std::wstring &value)
{
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0 || size > 4096 || value.find(L'\0') != std::wstring::npos)
        throw std::runtime_error("probe text must contain 1..4096 UTF-8 bytes without NUL");
    std::string text(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            text.data(), size, nullptr, nullptr) != size)
        throw std::runtime_error("cannot encode probe text as UTF-8");
    return text;
}

Options options(int argc, wchar_t **argv)
{
    Options out;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring key = argv[index];
        if (key == L"--help") { out.help = true; continue; }
        if (key == L"--stream") { out.stream = true; continue; }
        if (key == L"--realtime") { out.realtime = true; continue; }
        if (key == L"--prepare-tts") { out.prepare_tts = out.readback_probe = true; continue; }
        if (key == L"--readback-probe") { out.readback_probe = true; continue; }
        if (key == L"--final-only") { out.final_only = true; continue; }
        if (key == L"--tts-segments") { out.tts_segments = true; continue; }
        if (key == L"--tts-cold") { out.tts_segments = true; out.tts_cold = true; continue; }
        if (key == L"--tts-fast-sink")
        { out.tts_segments = true; out.tts_fast_sink = true; continue; }
        if (key == L"--tts-muted-playback")
        { out.tts_segments = true; out.tts_muted_playback = true; continue; }
        if (++index == argc) throw std::runtime_error("missing probe option value");
        const std::wstring value = argv[index];
        if (key == L"--pcm") out.pcm = value;
        else if (key == L"--pcm-rate")
        {
            if (value != L"16000" && value != L"24000")
                throw std::runtime_error("PCM rate must be 16000 or 24000");
            out.pcm_rate = value == L"24000" ? 24000 : 16000;
        }
        else if (key == L"--resampler")
        {
            if (value != L"linear" && value != L"sinc")
                throw std::runtime_error("probe resampler must be linear or sinc");
            out.resampler = value == L"sinc" ? hmi::probe::Resampler::Sinc : hmi::probe::Resampler::Linear;
        }
        else if (key == L"--tts-text") out.tts_text = option_text(value);
        else if (key == L"--asr-text") out.asr_text = option_text(value);
        else if (key == L"--expect-text") out.expect_text = option_text(value);
        else if (key == L"--tts-cache")
        {
            if (value != L"0" && value != L"1") throw std::runtime_error("TTS cache must be 0 or 1");
            out.tts_cache = value;
        }
        else if (key == L"--tts-idle-check")
        {
            std::size_t consumed = 0;
            out.tts_idle_check = std::stoi(value, &consumed);
            if (consumed != value.size() || out.tts_idle_check < 1 || out.tts_idle_check > 30)
                throw std::runtime_error("TTS idle check must be 1..30 seconds");
        }
        else if (key == L"--tts-perf-mode")
        {
            if (value != L"off" && value != L"flash_attention" && value != L"cuda_perf")
                throw std::runtime_error("TTS perf mode must be off, flash_attention, or cuda_perf");
            out.tts_perf_mode = value;
        }
        else if (key == L"--save-pcm") out.save_pcm = value;
        else if (key == L"--save-tts-pcm") out.save_tts_pcm = value;
        else if (key == L"--backend")
        {
            if (value != L"auto" && value != L"cuda" && value != L"vulkan" && value != L"cpu")
                throw std::runtime_error("invalid ASR backend");
            out.backend = value == L"auto" ? "auto" : value == L"cuda" ? "cuda" : value == L"vulkan" ? "vulkan" : "cpu";
        }
        else if (key == L"--threads")
        {
            std::size_t consumed = 0;
            out.threads = std::stoi(value, &consumed);
            if (consumed != value.size() || out.threads < 1 || out.threads > 256)
                throw std::runtime_error("threads must be 1..256");
        }
        else if (key == L"--iterations")
        {
            std::size_t consumed = 0;
            out.iterations = std::stoi(value, &consumed);
            if (consumed != value.size() || out.iterations < 1 || out.iterations > 20)
                throw std::runtime_error("iterations must be 1..20");
            out.iterations_set = true;
        }
        else if (key == L"--preview-interval" || key == L"--minimum-audio-seconds")
        {
            std::size_t consumed = 0;
            const double seconds = std::stod(value, &consumed);
            if (consumed != value.size() || !std::isfinite(seconds))
                throw std::runtime_error("probe audio interval must be finite");
            if (key == L"--preview-interval")
            {
                if (seconds < 0.01 || seconds > 30.0)
                    throw std::runtime_error("preview interval must be 0.01..30 seconds");
                out.preview_interval_seconds = seconds;
            }
            else
            {
                if (seconds < 0.0 || seconds > 29.9)
                    throw std::runtime_error("minimum audio duration must be 0..29.9 seconds");
                out.minimum_audio_seconds = seconds;
            }
        }
        else throw std::runtime_error("unknown probe option");
    }
    if (out.stream && out.final_only) throw std::runtime_error("--stream and --final-only are mutually exclusive");
    if (out.realtime && !out.stream) throw std::runtime_error("--realtime requires --stream");
    if (out.readback_probe && (!out.stream || !out.realtime || out.pcm.empty() || out.tts_segments || out.final_only))
        throw std::runtime_error("--readback-probe/--prepare-tts require --stream --realtime --pcm and exclude TTS-only/final-only modes");
    if (out.tts_fast_sink && out.tts_muted_playback)
        throw std::runtime_error("fast sink and muted playback are mutually exclusive");
    if (!out.save_tts_pcm.empty() && !out.tts_segments)
        throw std::runtime_error("--save-tts-pcm requires a TTS probe mode");
    if (out.tts_idle_check && !out.tts_segments)
        throw std::runtime_error("--tts-idle-check requires a TTS probe mode");
    return out;
}
double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void usage()
{
    std::printf(
        "Usage: hmi_voice_probe [options]\n"
        "  --backend auto|cuda|vulkan|cpu --threads 1..256 --iterations 1..20\n"
        "  --stream [--preview-interval 0.01..30] [--realtime] | --final-only\n"
        "  --readback-probe (requires --stream --realtime --pcm; cold first-segment PCM, never played)\n"
        "  --prepare-tts (implies --readback-probe; prepare the model after the first real preview)\n"
        "  --pcm FILE [--pcm-rate 16000|24000] | --asr-text TEXT --expect-text TEXT\n"
        "  --resampler linear|sinc (default: linear; sinc adds an anti-alias low-pass filter)\n"
        "  --minimum-audio-seconds 0..29.9 --save-pcm FILE\n"
        "  --tts-segments | --tts-fast-sink | --tts-muted-playback\n"
        "  --tts-text TEXT --tts-perf-mode off|flash_attention|cuda_perf --save-tts-pcm FILE\n"
        "  --tts-cold --tts-cache 0|1 --tts-idle-check 1..30\n"
        "PCM files are raw little-endian float32 mono, within the project directory; default rate is 16 kHz.\n"
        "ASR defaults to final-only, 3 iterations; TTS defaults to 2 iterations.\n"
        "File input is unchanged unless explicit minimum duration adds trailing silence.\n"
        "Synthetic streaming input repeats short speech to 6 seconds unless a minimum is supplied.\n"
        "TTS fast sink measures generation without playback or audio-duration waits.\n"
        "Saved TTS PCM is the whole-text baseline, or joined first-iteration segments with --tts-cold.\n");
}

std::uint64_t pcm_fingerprint(const std::vector<float> &pcm)
{
    std::uint64_t hash = 14695981039346656037ULL;
    const auto *bytes = reinterpret_cast<const unsigned char *>(pcm.data());
    for (std::size_t index = 0; index < pcm.size() * sizeof(float); ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

void save_pcm(const std::filesystem::path &path, const std::vector<float> &pcm)
{
    if (path.empty()) return;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.write(reinterpret_cast<const char *>(pcm.data()),
            static_cast<std::streamsize>(pcm.size() * sizeof(float))))
        throw std::runtime_error("cannot save probe PCM");
    output.close();
    if (!output) throw std::runtime_error("cannot finish saving probe PCM");
}

double median(std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    const std::size_t middle = samples.size() / 2;
    return samples.size() % 2 ? samples[middle] : (samples[middle - 1] + samples[middle]) / 2.0;
}

void sample_summary(const char *kind, const std::vector<double> &samples)
{
    if (samples.empty()) return;
    double total = 0.0;
    for (const double sample : samples) total += sample;
    const std::vector<double> warm(samples.begin() + 1, samples.end());
    std::printf("PROBE summary kind=%s samples=%zu first_ms=%.3f mean_ms=%.3f median_ms=%.3f "
        "warm_samples=%zu warm_median_ms=%.3f\n", kind, samples.size(), samples.front(),
        total / static_cast<double>(samples.size()), median(samples), warm.size(),
        warm.empty() ? 0.0 : median(warm));
}

void loaded_sdk_paths()
{
    for (const wchar_t *name : {L"native.dll", L"ggml.dll", L"ggml-base.dll", L"ggml-cuda.dll", L"ggml-vulkan.dll"})
    {
        const HMODULE module = GetModuleHandleW(name);
        if (!module) continue;
        wchar_t path[32768]{};
        const DWORD count = GetModuleFileNameW(module, path, 32768);
        if (count == 0 || count >= 32768) continue;
        const int size = WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
        if (size <= 0) continue;
        std::string utf8(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(count), utf8.data(), size, nullptr, nullptr);
        std::printf("PROBE module=%s\n", utf8.c_str());
    }
    std::fflush(stdout);
}

void memory(const char *stage)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
        std::printf("PROBE memory stage=%s working_mib=%.1f peak_mib=%.1f private_mib=%.1f\n", stage,
                    static_cast<double>(counters.WorkingSetSize) / 1048576.0,
                    static_cast<double>(counters.PeakWorkingSetSize) / 1048576.0,
                    static_cast<double>(counters.PrivateUsage) / 1048576.0);
    std::fflush(stdout);
}

double process_cpu_ms()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
    const auto ticks = [](const FILETIME &time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
}

class RealtimeFeed
{
public:
    struct Chunk
    {
        std::size_t cursor = 0;
        std::size_t frames = 0;
    };
    struct Statistics
    {
        std::size_t high_water_chunks = 0;
        std::size_t high_water_frames = 0;
        std::size_t dropped_chunks = 0;
        std::size_t dropped_frames = 0;
        double maximum_lateness_ms = 0.0;
        Clock::time_point eof{};
    };

    RealtimeFeed(std::size_t frames, Clock::time_point start)
    {
        if (frames == 0) { finished_ = true; return; }
        worker_ = std::thread([this, frames, start] {
            for (std::size_t cursor = 0; cursor < frames; cursor += 4000)
            {
                const auto count = std::min(std::size_t{4000}, frames - cursor);
                const auto deadline = start + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(static_cast<double>(cursor + count) / 16000.0));
                std::unique_lock lock(mutex_);
                if (changed_.wait_until(lock, deadline, [this] { return stopping_; })) return;
                statistics_.maximum_lateness_ms = std::max(statistics_.maximum_lateness_ms,
                    std::chrono::duration<double, std::milli>(Clock::now() - deadline).count());
                if (queued_ == chunks_.size())
                {
                    ++statistics_.dropped_chunks;
                    statistics_.dropped_frames += count;
                }
                else
                {
                    chunks_[(read_ + queued_) % chunks_.size()] = {cursor, count};
                    ++queued_;
                    queued_frames_ += count;
                    statistics_.high_water_chunks = std::max(statistics_.high_water_chunks, queued_);
                    statistics_.high_water_frames = std::max(statistics_.high_water_frames, queued_frames_);
                }
                if (cursor + count == frames)
                {
                    statistics_.eof = Clock::now();
                    finished_ = true;
                }
                lock.unlock();
                changed_.notify_all();
            }
        });
    }

    ~RealtimeFeed()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    RealtimeFeed(const RealtimeFeed &) = delete;
    RealtimeFeed &operator=(const RealtimeFeed &) = delete;

    bool next(Chunk &chunk)
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return stopping_ || finished_ || queued_ != 0; });
        if (queued_ == 0) return false;
        chunk = chunks_[read_];
        read_ = (read_ + 1) % chunks_.size();
        --queued_;
        queued_frames_ -= chunk.frames;
        return true;
    }

    Statistics statistics()
    {
        std::lock_guard lock(mutex_);
        return statistics_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Chunk, 120> chunks_{};
    std::size_t read_ = 0;
    std::size_t queued_ = 0;
    std::size_t queued_frames_ = 0;
    Statistics statistics_;
    bool stopping_ = false;
    bool finished_ = false;
    std::thread worker_;
};

}

namespace
{
int tts_segment_probe(hmi::engine::Client &client, const Options &selected)
{
    hmi::engine::TtsSession tts;
    hmi::engine::TtsConfig config;
    config.backend = selected.backend;
    config.threads = selected.threads;
    std::string error;
    if (!tts.open(client, config, &error)) throw std::runtime_error(error);
    hmi::audio::AudioBuffer audio;
    auto started = Clock::now();
    double baseline = 0.0;
    if (!selected.tts_cold)
    {
        if (!tts.synthesize("你好", audio, &error)) throw std::runtime_error(error);
        std::printf("PROBE TTS warmup_ms=%.3f sdk_openmp=%d\n", elapsed(started), HMI_AUDIOCPP_SDK_OPENMP_ENABLED);
        started = Clock::now();
        if (!tts.synthesize(selected.tts_text, audio, &error)) throw std::runtime_error(error);
        baseline = elapsed(started);
        std::printf("PROBE TTS whole_text pcm_ready_ms=%.3f duration_s=%.3f text_bytes=%zu\n", baseline,
            static_cast<double>(audio.samples.size()) / (audio.sample_rate * audio.channels), selected.tts_text.size());
        if (!selected.save_tts_pcm.empty())
        {
            const auto pcm = hmi::probe::mono_16k(audio, selected.resampler);
            save_pcm(selected.save_tts_pcm, pcm);
            std::printf("PROBE TTS saved source=whole_text frames=%zu sample_rate=16000 channels=1 "
                "fnv1a64=%016llx\n", pcm.size(), static_cast<unsigned long long>(pcm_fingerprint(pcm)));
        }
    }
    std::fflush(stdout);
    const int iterations = selected.iterations_set ? selected.iterations : 2;
    std::vector<double> first_audio_times;
    std::vector<double> synthesis_times;
    std::vector<double> wall_times;
    for (int iteration = 1; iteration <= iterations; ++iteration)
    {
        hmi::engine::detail::TtsPipelineStatistics statistics;
        std::size_t chunks = 0;
        double duration = 0.0;
        std::vector<float> joined_pcm;
        const auto record_pcm = [&](const hmi::audio::AudioBuffer &chunk) {
            if (!selected.tts_cold || iteration != 1 || selected.save_tts_pcm.empty()) return;
            const auto converted = hmi::probe::mono_16k(chunk, selected.resampler);
            joined_pcm.insert(joined_pcm.end(), converted.begin(), converted.end());
        };
        const auto pipeline_start = Clock::now();
        const bool ok = hmi::engine::detail::run_tts_pipeline(selected.tts_text,
            [&tts](std::string_view text, hmi::audio::AudioBuffer &out, std::string *detail) {
                const bool generated = tts.synthesize(text, out, detail);
                const auto state = tts.statistics();
                std::printf("PROBE TTS generation text_bytes=%zu max_tokens=%d retries=%zu limit_hits=%zu finish_reason=%s ok=%d\n",
                    text.size(), state.max_tokens, state.generation_retries, state.limit_hits,
                    state.last_finish_reason.c_str(), generated ? 1 : 0);
                return generated;
            },
            [&](const hmi::audio::detail::ReadAudio &read, std::string *detail,
                const std::function<bool()> &cancelled) {
                if (selected.tts_muted_playback)
                {
                    return hmi::audio::detail::play_stream(
                        [&](hmi::audio::AudioBuffer &chunk, std::string *source_error) {
                            const auto state = read(chunk, source_error);
                            if (state == hmi::audio::detail::StreamRead::Ready)
                            {
                                record_pcm(chunk);
                                const double seconds = static_cast<double>(chunk.samples.size()) /
                                    (chunk.sample_rate * chunk.channels);
                                duration += seconds;
                                ++chunks;
                                std::printf("PROBE TTS segment iteration=%d chunk=%zu ready_ms=%.3f audio_s=%.3f sink=wasapi_silence\n",
                                    iteration, chunks, elapsed(pipeline_start), seconds);
                                std::fflush(stdout);
                                std::fill(chunk.samples.begin(), chunk.samples.end(), 0.0f);
                            }
                            return state;
                        }, detail, cancelled);
                }
                for (;;)
                {
                    if (cancelled()) { if (detail) *detail = "probe cancelled"; return false; }
                    hmi::audio::AudioBuffer chunk;
                    const auto state = read(chunk, detail);
                    if (state == hmi::audio::detail::StreamRead::Failed) return false;
                    if (state == hmi::audio::detail::StreamRead::Finished) return true;
                    if (state == hmi::audio::detail::StreamRead::Pending)
                    { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
                    if (chunk.samples.empty() || chunk.sample_rate <= 0 || chunk.channels <= 0) return false;
                    record_pcm(chunk);
                    const double seconds = static_cast<double>(chunk.samples.size()) /
                        (chunk.sample_rate * chunk.channels);
                    duration += seconds;
                    ++chunks;
                    std::printf("PROBE TTS segment iteration=%d chunk=%zu ready_ms=%.3f audio_s=%.3f sink=%s\n",
                        iteration, chunks, elapsed(pipeline_start), seconds,
                        selected.tts_fast_sink ? "discard_no_audio" : "clock_no_audio");
                    std::fflush(stdout);
                    if (selected.tts_fast_sink) continue;
                    const auto until = Clock::now() + std::chrono::duration<double>(seconds);
                    while (Clock::now() < until)
                    {
                        if (cancelled()) { if (detail) *detail = "probe cancelled"; return false; }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
            }, [] { return false; }, &error, &statistics);
        const bool complete = ok && chunks > 0 && chunks == statistics.generated_segments &&
            chunks == statistics.segments && statistics.queue_high_water <= 2;
        std::printf("PROBE TTS %s iteration=%d segments=%zu chunks=%zu first_pcm_ms=%.3f synthesis_ms=%.3f "
            "queue_high_water=%zu producer_wait_ms=%.3f wall_ms=%.3f audio_s=%.3f whole_pcm_ms=%.3f\n",
            complete ? "PASS" : "FAIL", iteration,
            statistics.segments, chunks, statistics.first_audio_ms, statistics.synthesis_ms,
            statistics.queue_high_water, statistics.producer_wait_ms, statistics.total_ms, duration, baseline);
        const auto session_state = tts.statistics();
        std::printf("PROBE TTS state iteration=%d model_ready=%d model_load_ms=%.3f cache_hits=%zu cache_bytes=%zu "
            "sampling=%s last_sdk_max_tokens=%d generation_retries=%zu limit_hits=%zu last_finish_reason=%s\n",
            iteration, session_state.model_ready ? 1 : 0, session_state.model_load_ms,
            session_state.cache_hits, session_state.cache_bytes,
            session_state.sampling_enabled ? "enabled" : "greedy", session_state.max_tokens,
            session_state.generation_retries, session_state.limit_hits, session_state.last_finish_reason.c_str());
        std::fflush(stdout);
        if (!complete)
            throw std::runtime_error(error.empty() ? "TTS did not consume every segment within its queue bound" : error);
        if (!joined_pcm.empty())
        {
            save_pcm(selected.save_tts_pcm, joined_pcm);
            std::printf("PROBE TTS saved source=joined_segments frames=%zu fnv1a64=%016llx\n",
                joined_pcm.size(), static_cast<unsigned long long>(pcm_fingerprint(joined_pcm)));
        }
        first_audio_times.push_back(statistics.first_audio_ms);
        synthesis_times.push_back(statistics.synthesis_ms);
        wall_times.push_back(statistics.total_ms);
    }
    sample_summary("tts_first_pcm", first_audio_times);
    sample_summary("tts_synthesis", synthesis_times);
    sample_summary("tts_wall", wall_times);
    std::fflush(stdout);
    if (selected.tts_idle_check)
    {
        const auto segments = hmi::engine::detail::split_tts_text(selected.tts_text);
        if (segments.empty()) throw std::runtime_error("idle check requires spoken text");
        if (!tts.synthesize(segments.front(), audio, &error)) throw std::runtime_error(error);
        const auto reference_fingerprint = pcm_fingerprint(hmi::probe::mono_16k(audio, selected.resampler));
        memory("tts_before_idle");
        const auto deadline = Clock::now() + std::chrono::seconds(selected.tts_idle_check + 5);
        while (tts.statistics().model_ready && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto state = tts.statistics();
        memory("tts_after_idle");
        std::printf("PROBE TTS idle model_ready=%d idle_unloads=%llu cache_entries=%zu cache_bytes=%zu\n",
            state.model_ready ? 1 : 0, static_cast<unsigned long long>(state.idle_unloads),
            state.cache_entries, state.cache_bytes);
        if (state.model_ready || state.idle_unloads == 0) throw std::runtime_error("TTS did not unload after idle timeout");
        started = Clock::now();
        if (!tts.synthesize(segments.front(), audio, &error)) throw std::runtime_error(error);
        state = tts.statistics();
        const bool same_pcm = pcm_fingerprint(hmi::probe::mono_16k(audio, selected.resampler)) == reference_fingerprint;
        std::printf("PROBE TTS replay pcm_ms=%.3f model_ready=%d cache_hits=%zu same_pcm=%d\n",
            elapsed(started), state.model_ready ? 1 : 0, state.last_request_cache_hits, same_pcm ? 1 : 0);
        if (selected.tts_cache == L"1" && (!same_pcm || state.model_ready))
            throw std::runtime_error("cached replay changed PCM or reloaded the model");
        started = Clock::now();
        if (!tts.synthesize(segments.front() + "接下来继续。", audio, &error)) throw std::runtime_error(error);
        state = tts.statistics();
        std::printf("PROBE TTS new_text pcm_ms=%.3f model_ready=%d model_load_ms=%.3f frames=%zu\n",
            elapsed(started), state.model_ready ? 1 : 0, state.model_load_ms,
            audio.samples.size() / static_cast<std::size_t>(audio.channels));
        if (!state.model_ready) throw std::runtime_error("new text did not reload the TTS model");
        memory("tts_after_reload");
    }
    tts.stop();
    return 0;
}

int run(const Options &selected)
{
    SetConsoleOutputCP(CP_UTF8);
    if (selected.help) { usage(); return 0; }
    for (const auto &path : {selected.pcm, selected.save_pcm, selected.save_tts_pcm})
    {
        std::filesystem::path resolved;
        std::string detail;
        if (!path.empty() && !hmi::base::resolve_project_path(std::filesystem::absolute(path),
                hmi::base::process_directory(), resolved, &detail)) throw std::runtime_error(detail);
    }
    if (!selected.tts_perf_mode.empty() &&
        !SetEnvironmentVariableW(L"HMI_TTS_PERF_MODE", selected.tts_perf_mode.c_str()))
        throw std::runtime_error("cannot set process-local TTS perf mode");
    if (!selected.tts_cache.empty() && !SetEnvironmentVariableW(L"HMI_TTS_PCM_CACHE", selected.tts_cache.c_str()))
        throw std::runtime_error("cannot set process-local TTS cache option");
    if (selected.tts_idle_check && !SetEnvironmentVariableW(L"HMI_TTS_IDLE_SECONDS", std::to_wstring(selected.tts_idle_check).c_str()))
        throw std::runtime_error("cannot set process-local TTS idle timeout");
    wchar_t perf_mode[32]{};
    const DWORD perf_count = GetEnvironmentVariableW(L"HMI_TTS_PERF_MODE", perf_mode, 32);
    if (perf_count >= 32) throw std::runtime_error("TTS perf mode environment value is too long");
    const std::string requested_perf = perf_count ? option_text(std::wstring(perf_mode, perf_count)) : "default";
    std::printf("PROBE settings requested_backend=%s threads=%d iterations=%d requested_tts_perf_mode=%s "
        "prepare_tts=%d readback_probe=%d\n",
        selected.backend.c_str(), selected.threads,
        selected.tts_segments && !selected.iterations_set ? 2 : selected.iterations, requested_perf.c_str(),
        selected.prepare_tts ? 1 : 0, selected.readback_probe ? 1 : 0);
    hmi::engine::Client client;
    std::string error;
    std::printf("PROBE starting engine\n"); std::fflush(stdout);
    if (!client.open(&error))
    {
        std::printf("PROBE FAIL engine: %s\n", error.c_str());
        return 1;
    }
    std::printf("PROBE abi=%u build=%s\n", client.version().abi, client.version().build.c_str());
    const auto openmp = hmi::engine::detail::loaded_openmp_thread_api();
    const int inherited_threads = openmp.get_max_threads ? openmp.get_max_threads() : 0;
    {
        const hmi::engine::detail::ScopedSdkThreads limit(selected.threads, openmp);
        std::printf("PROBE openmp inherited_max_threads=%d scoped_max_threads=%d "
            "scope=calling_worker_limit\n", inherited_threads, limit.maximum_threads());
    }
    loaded_sdk_paths();
    memory("engine_ready");
    if (selected.tts_segments) return tts_segment_probe(client, selected);

    hmi::audio::AudioBuffer generated;
    if (selected.pcm.empty())
    {
        hmi::engine::TtsSession tts;
        hmi::engine::TtsConfig config;
        config.backend = selected.backend;
        config.threads = selected.threads;
        const auto start = Clock::now();
        std::printf("PROBE loading TTS and synthesizing\n"); std::fflush(stdout);
        if (!tts.open(client, config, &error) || !tts.synthesize(selected.asr_text, generated, &error))
        {
            std::printf("PROBE FAIL TTS: %s\n", error.c_str()); std::fflush(stdout);
            return 2;
        }
        std::printf("PROBE TTS ms=%.3f frames=%zu sample_rate=%d channels=%d\n", elapsed(start),
                    generated.samples.size() / static_cast<std::size_t>(generated.channels),
                    generated.sample_rate, generated.channels);
        memory("tts_synthesized");
        tts.stop();
    }
    memory("tts_released");
    if (!selected.pcm.empty())
    {
        const auto bytes = std::filesystem::file_size(selected.pcm);
        if (bytes == 0 || bytes > static_cast<std::uintmax_t>(selected.pcm_rate) * 30 * sizeof(float) ||
            bytes % sizeof(float) != 0)
            throw std::runtime_error("source PCM must contain float32 mono audio within 30 seconds at its declared rate");
        generated.samples.resize(static_cast<std::size_t>(bytes / sizeof(float)));
        generated.sample_rate = selected.pcm_rate;
        generated.channels = 1;
        std::ifstream input(selected.pcm, std::ios::binary);
        if (!input.read(reinterpret_cast<char *>(generated.samples.data()), static_cast<std::streamsize>(bytes)))
            throw std::runtime_error("cannot read probe PCM");
    }
    if (generated.sample_rate < 8000 || generated.sample_rate > 192000 || generated.channels < 1 ||
        generated.channels > 8 || generated.samples.empty() ||
        generated.samples.size() % static_cast<std::size_t>(generated.channels) != 0 ||
        generated.samples.size() / static_cast<std::size_t>(generated.channels) >
            static_cast<std::size_t>(generated.sample_rate) * 30 ||
        !std::all_of(generated.samples.begin(), generated.samples.end(), [](float value) { return std::isfinite(value); }))
        throw std::runtime_error("source probe PCM must contain non-empty finite frames within 30 seconds at its declared format");
    auto pcm = hmi::probe::mono_16k(generated, selected.resampler);
    if (pcm.empty() || pcm.size() > 16000 * 30 ||
        !std::all_of(pcm.begin(), pcm.end(), [](float value) { return std::isfinite(value); }))
        throw std::runtime_error("probe PCM must contain 1..480000 finite float32 mono samples at 16 kHz");
    const auto source_frames = generated.samples.size() / static_cast<std::size_t>(generated.channels);
    std::printf("PROBE conversion resampler=%s source_sample_rate=%d source_channels=%d source_frames=%zu "
        "source_duration_s=%.6f source_fnv1a64=%016llx output_sample_rate=16000 output_frames=%zu "
        "output_duration_s=%.6f output_fnv1a64=%016llx\n", hmi::probe::resampler_name(selected.resampler),
        generated.sample_rate, generated.channels, source_frames,
        static_cast<double>(source_frames) / generated.sample_rate,
        static_cast<unsigned long long>(pcm_fingerprint(generated.samples)), pcm.size(),
        static_cast<double>(pcm.size()) / 16000.0, static_cast<unsigned long long>(pcm_fingerprint(pcm)));
    const std::size_t original_frames = pcm.size();
    if (selected.minimum_audio_seconds >= 0.0)
    {
        const auto minimum_frames = static_cast<std::size_t>(std::ceil(selected.minimum_audio_seconds * 16000.0));
        if (pcm.size() < minimum_frames) pcm.resize(minimum_frames, 0.0f);
    }
    else if (selected.stream && selected.pcm.empty() && pcm.size() < 16000 * 6)
    {
        const auto phrase = pcm;
        while (pcm.size() < 16000 * 6)
        {
            pcm.insert(pcm.end(), 1600, 0.0f);
            pcm.insert(pcm.end(), phrase.begin(), phrase.end());
        }
    }
    if (!selected.stream && pcm.size() >= 16000 * 30)
        throw std::runtime_error("final-only diagnostic requires audio shorter than 30 seconds");
    save_pcm(selected.save_pcm, pcm);
    std::printf("PROBE resampled frames=%zu original_frames=%zu added_frames=%zu duration_s=%.3f "
        "fnv1a64=%016llx source=%s augmentation=%s\n", pcm.size(), original_frames,
        pcm.size() - original_frames, static_cast<double>(pcm.size()) / 16000.0,
        static_cast<unsigned long long>(pcm_fingerprint(pcm)), selected.pcm.empty() ? "tts" : "file",
        selected.minimum_audio_seconds >= 0.0 ? "explicit_silence" :
        pcm.size() != original_frames ? "synthetic_phrase_repeat" : "none");
    std::fflush(stdout);
    hmi::engine::AsrSession asr;
    const auto load_start = Clock::now();
    hmi::engine::AsrConfig asr_config;
    asr_config.backend = selected.backend;
    asr_config.threads = selected.threads;
    asr_config.preview_interval_seconds = selected.stream ? selected.preview_interval_seconds : 30.0;
    if (!asr.open(client, asr_config, &error))
    {
        std::printf("PROBE FAIL ASR open: %s\n", error.c_str());
        return 3;
    }
    std::printf("PROBE ASR load_ms=%.3f preferred_chunk=%zu\n", elapsed(load_start), asr.preferred_chunk_samples());
    std::printf("PROBE backend=%s threads=%d sdk_openmp=%d mode=%s preview_interval_s=%.3f realtime_paced=%d\n",
        asr.statistics().backend.c_str(), selected.threads, HMI_AUDIOCPP_SDK_OPENMP_ENABLED,
        selected.stream ? "prefix_preview_refine" : "final", asr_config.preview_interval_seconds, selected.realtime ? 1 : 0);
    memory("asr_ready");
    int failures = 0;
    std::vector<double> total_times;
    std::vector<double> finish_times;
    std::vector<double> preview_times;
    std::vector<double> refine_times;
    for (int iteration = 1; iteration <= selected.iterations; ++iteration)
    {
        hmi::engine::TtsSession prepared_tts;
        if (selected.readback_probe)
        {
            hmi::engine::TtsConfig tts_config;
            tts_config.backend = selected.backend;
            tts_config.threads = selected.threads;
            if (!prepared_tts.open(client, tts_config, &error)) throw std::runtime_error(error);
            std::printf("PROBE readback_service iteration=%d model_ready=%d prepare_early=%d mode=cold\n",
                iteration, prepared_tts.statistics().model_ready ? 1 : 0, selected.prepare_tts ? 1 : 0);
        }
        memory("asr_before_feed");
        const double cpu_start_ms = process_cpu_ms();
        const auto start = Clock::now();
        if (!asr.begin(&error))
        {
            std::printf("PROBE FAIL iteration=%d begin: %s\n", iteration, error.c_str());
            return 4;
        }
        std::vector<hmi::engine::Transcript> partial;
        std::size_t nonempty_previews = 0;
        double first_nonempty_ms = -1.0;
        double last_nonempty_ms = -1.0;
        double maximum_gap_ms = -1.0;
        double last_publication_ms = -1.0;
        double maximum_publication_gap_ms = -1.0;
        bool prepare_queued = false;
        double prepare_queued_ms = -1.0;
        std::size_t fed_frames = 0;
        RealtimeFeed feeder(selected.realtime ? pcm.size() : 0, start);
        for (std::size_t next_cursor = 0;;)
        {
            RealtimeFeed::Chunk chunk;
            if (selected.realtime)
            {
                if (!feeder.next(chunk)) break;
            }
            else
            {
                if (next_cursor >= pcm.size()) break;
                chunk = {next_cursor, std::min(std::size_t{160}, pcm.size() - next_cursor)};
                next_cursor += chunk.frames;
            }
            const auto before = partial.size();
            if (!asr.push(pcm.data() + chunk.cursor, chunk.frames, partial, &error))
            {
                std::printf("PROBE FAIL iteration=%d push: %s\n", iteration, error.c_str());
                return 5;
            }
            fed_frames += chunk.frames;
            for (std::size_t index = before; index < partial.size(); ++index)
            {
                const double wall_ms = elapsed(start);
                double gap_ms = -1.0;
                if (last_publication_ms >= 0.0)
                    maximum_publication_gap_ms = std::max(maximum_publication_gap_ms, wall_ms - last_publication_ms);
                last_publication_ms = wall_ms;
                if (!partial[index].text.empty())
                {
                    ++nonempty_previews;
                    if (first_nonempty_ms < 0.0) first_nonempty_ms = wall_ms;
                    if (last_nonempty_ms >= 0.0) gap_ms = wall_ms - last_nonempty_ms;
                    last_nonempty_ms = wall_ms;
                    maximum_gap_ms = std::max(maximum_gap_ms, gap_ms);
                }
                std::printf("PROBE preview iteration=%d audio_ms=%zu revision=%zu text_bytes=%zu final=%d wall_ms=%.3f gap_ms=%.3f\n",
                    iteration, (chunk.cursor + chunk.frames) / 16, index + 1,
                    partial[index].text.size(), partial[index].final ? 1 : 0, wall_ms, gap_ms);
                std::fflush(stdout);
                if (partial[index].final) ++failures;
                if (selected.prepare_tts && !prepare_queued && !partial[index].final &&
                    partial[index].text.size() <= 4096 && partial[index].text.find('\0') == std::string::npos &&
                    partial[index].text.find_first_not_of(" \t\r\n") != std::string::npos &&
                    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, partial[index].text.data(),
                        static_cast<int>(partial[index].text.size()), nullptr, 0) > 0 &&
                    elapsed(start) < static_cast<double>(pcm.size()) / 16.0)
                {
                    prepare_queued = true;
                    prepare_queued_ms = elapsed(start);
                    const bool accepted = prepared_tts.prepare(static_cast<std::uint64_t>(iteration));
                    std::printf("PROBE prepare_queued iteration=%d request_id=%d wall_ms=%.3f accepted=%d text_bytes=%zu\n",
                        iteration, iteration, prepare_queued_ms, accepted ? 1 : 0, partial[index].text.size());
                    std::fflush(stdout);
                    if (!accepted) throw std::runtime_error("TTS model preparation request was rejected");
                }
            }
        }
        hmi::engine::Transcript transcript;
        const auto finish_start = Clock::now();
        if (!asr.finish(transcript, &error))
        {
            std::printf("PROBE FAIL iteration=%d finish: %s\n", iteration, error.c_str());
            return 6;
        }
        const auto final_at = Clock::now();
        const double cpu_final_ms = process_cpu_ms();
        const double finish_ms = std::chrono::duration<double, std::milli>(final_at - finish_start).count();
        const double total_ms = std::chrono::duration<double, std::milli>(final_at - start).count();
        const auto feed_statistics = feeder.statistics();
        const auto prepared_state = prepared_tts.statistics();
        const auto statistics = asr.statistics();
        PROCESS_MEMORY_COUNTERS_EX memory_at_final{};
        memory_at_final.cb = sizeof(memory_at_final);
        const bool have_final_memory = GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory_at_final), sizeof(memory_at_final)) != FALSE;
        hmi::audio::AudioBuffer readback_audio;
        hmi::engine::TtsStatistics readback_state;
        double first_pcm_from_final_ms = -1.0;
        std::size_t readback_text_bytes = 0;
        std::size_t readback_busy_rejections = 0;
        if (selected.readback_probe)
        {
            const auto segments = hmi::engine::detail::split_tts_text(transcript.text);
            if (segments.empty()) throw std::runtime_error("ASR final has no spoken segment for the readback probe");
            readback_text_bytes = segments.front().size();
            const auto deadline = final_at + std::chrono::seconds(45);
            std::string readback_error;
            for (;;)
            {
                if (Clock::now() >= deadline)
                    throw std::runtime_error("readback probe exceeded its 45-second preparation wait deadline");
                if (prepared_tts.synthesize(segments.front(), readback_audio, &readback_error)) break;
                // Diagnostic-only retry of our own immediate rejection; SDK failures are never retried here.
                if (readback_error != "TTS is busy; synchronous synthesis was not started")
                    throw std::runtime_error(readback_error.empty() ? "readback synthesis failed" : readback_error);
                ++readback_busy_rejections;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            first_pcm_from_final_ms = std::chrono::duration<double, std::milli>(Clock::now() - final_at).count();
            readback_state = prepared_tts.statistics();
            if (readback_audio.samples.empty() || readback_audio.sample_rate <= 0 || readback_audio.channels <= 0)
                throw std::runtime_error("readback synthesis returned no owned PCM");
        }
        const bool preview_due = selected.stream && pcm.size() >=
            static_cast<std::size_t>(std::ceil(selected.preview_interval_seconds * 16000.0));
        const bool matched = transcript.final && transcript.text.find(selected.expect_text) != std::string::npos &&
            (!preview_due || !partial.empty()) && (selected.stream || statistics.preview_count == 0);
        std::printf("PROBE %s iteration=%d total_ms=%.3f finish_ms=%.3f confidence=unknown text=%s\n",
                    matched ? "PASS" : "FAIL", iteration, total_ms, finish_ms, transcript.text.c_str());
        std::fflush(stdout);
        std::printf("PROBE timing iteration=%d previews=%zu preview_ms=%.3f refine_ms=%.3f rtf=%.4f\n",
            iteration, statistics.preview_count, statistics.preview_ms, statistics.refine_ms,
            (statistics.preview_ms + statistics.refine_ms) / (static_cast<double>(pcm.size()) / 16.0));
        std::printf("PROBE publication iteration=%d nonempty_previews=%zu first_nonempty_ms=%.3f "
            "max_nonempty_gap_ms=%.3f max_publication_gap_ms=%.3f origin=feed_started\n",
            iteration, nonempty_previews, first_nonempty_ms, maximum_gap_ms, maximum_publication_gap_ms);
        const double cpu_ms = cpu_start_ms >= 0.0 && cpu_final_ms >= cpu_start_ms ? cpu_final_ms - cpu_start_ms : -1.0;
        std::printf("PROBE cpu iteration=%d process_cpu_ms=%.3f wall_ms=%.3f one_core_percent=%.3f\n",
            iteration, cpu_ms, total_ms, cpu_ms >= 0.0 && total_ms > 0.0 ? cpu_ms * 100.0 / total_ms : -1.0);
        if (selected.realtime)
        {
            const double eof_ms = std::chrono::duration<double, std::milli>(feed_statistics.eof - start).count();
            const double final_delay_ms = std::chrono::duration<double, std::milli>(final_at - feed_statistics.eof).count();
            std::printf("PROBE feed iteration=%d chunk_frames=4000 queue_capacity_chunks=120 queue_capacity_frames=480000 "
                "queue_high_water_chunks=%zu queue_high_water_frames=%zu dropped_chunks=%zu dropped_frames=%zu "
                "fed_frames=%zu eof_ms=%.3f final_delay_from_eof_ms=%.3f maximum_lateness_ms=%.3f\n",
                iteration, feed_statistics.high_water_chunks, feed_statistics.high_water_frames,
                feed_statistics.dropped_chunks, feed_statistics.dropped_frames, fed_frames,
                eof_ms, final_delay_ms, feed_statistics.maximum_lateness_ms);
            if (feed_statistics.dropped_frames != 0 || fed_frames != pcm.size()) ++failures;
        }
        if (selected.prepare_tts)
        {
            std::printf("PROBE prepare_at_asr_final iteration=%d trigger=%s queued=%d queued_wall_ms=%.3f "
                "asr_final_wall_ms=%.3f model_ready=%d backend=%s generated_segments=%zu cache_entries=%zu\n",
                iteration, prepare_queued ? "queued" : "not_triggered", prepare_queued ? 1 : 0, prepare_queued_ms, total_ms,
                prepared_state.model_ready ? 1 : 0, prepared_state.backend.c_str(),
                prepared_state.generated_segments, prepared_state.cache_entries);
            if (prepared_state.generated_segments != 0 || prepared_state.cache_entries != 0) ++failures;
        }
        if (selected.readback_probe)
        {
            std::printf("PROBE readback iteration=%d first_pcm_from_asr_final_ms=%.3f ready_at_final=%d "
                "model_load_ms=%.3f first_segment_text_bytes=%zu frames=%zu sample_rate=%d channels=%d "
                "fnv1a64=%016llx busy_rejections=%zu prepare_trigger=%s sink=owned_pcm_no_audio\n",
                iteration, first_pcm_from_final_ms, prepared_state.model_ready ? 1 : 0,
                readback_state.model_load_ms, readback_text_bytes,
                readback_audio.samples.size() / static_cast<std::size_t>(readback_audio.channels),
                readback_audio.sample_rate, readback_audio.channels,
                static_cast<unsigned long long>(pcm_fingerprint(readback_audio.samples)), readback_busy_rejections,
                !selected.prepare_tts ? "disabled" : prepare_queued ? "queued" : "not_triggered");
        }
        total_times.push_back(total_ms);
        finish_times.push_back(finish_ms);
        preview_times.push_back(statistics.preview_ms);
        refine_times.push_back(statistics.refine_ms);
        if (!matched) ++failures;
        if (have_final_memory)
            std::printf("PROBE memory stage=asr_iteration working_mib=%.1f peak_mib=%.1f private_mib=%.1f\n",
                static_cast<double>(memory_at_final.WorkingSetSize) / 1048576.0,
                static_cast<double>(memory_at_final.PeakWorkingSetSize) / 1048576.0,
                static_cast<double>(memory_at_final.PrivateUsage) / 1048576.0);
        std::fflush(stdout);
        prepared_tts.stop();
        if (selected.readback_probe) memory("tts_readback_released");
    }
    asr.close();
    memory("asr_released");
    sample_summary("asr_total", total_times);
    sample_summary("asr_finish", finish_times);
    sample_summary("asr_preview", preview_times);
    sample_summary("asr_refine", refine_times);
    std::printf("PROBE complete failures=%d\n", failures);
    return failures == 0 ? 0 : 7;
}
}

int wmain(int argc, wchar_t **argv)
{
    try { return run(options(argc, argv)); }
    catch (const std::exception &failure)
    {
        std::fprintf(stderr, "PROBE FAIL %s\n", failure.what());
        return 8;
    }
}
