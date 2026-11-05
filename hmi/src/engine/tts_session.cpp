#include "engine/tts_session.hpp"
#include "engine/sdk_library.hpp"
#include "engine/sdk_session.hpp"
#include "engine/sdk_threads.hpp"
#include "base/project_paths.hpp"
#include "base/logger.hpp"
#include "engine/tts_pipeline.hpp"
#include "util/json_writer.hpp"
#include "audio/playback_diagnostics.hpp"

#include <audiocpp.h>
#include <windows.h>
#include <mmreg.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <chrono>
#include <deque>
#include <algorithm>

namespace hmi::engine
{
namespace
{
bool valid_text(std::string_view text)
{
    return !text.empty() && text.size() <= 4096 && text.find('\0') == std::string_view::npos &&
           text.find_first_not_of(" \t\r\n") != std::string_view::npos &&
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                               nullptr, 0) > 0;
}

std::string utterance_language(std::string_view text, const std::string &configured)
{
    // Keep mixed scripts on the configured language; detect only ASCII Latin speech.
    if (configured != "zh") return configured;
    using namespace detail::tts_segmentation;
    std::vector<Codepoint> points;
    if (!decode(text, points)) return configured;
    bool has_latin = false;
    for (const auto &point : points)
    {
        if (ascii_letter(point.value)) { has_latin = true; continue; }
        if (point.value < 0x80U && ((point.value >= '0' && point.value <= '9') || !spoken(point.value))) continue;
        if (point.value <= 0xffffU)
        {
            const wchar_t unit = static_cast<wchar_t>(point.value);
            WORD category = 0;
            if (GetStringTypeW(CT_CTYPE1, &unit, 1, &category) && (category & (C1_SPACE | C1_PUNCT)) != 0) continue;
        }
        return configured;
    }
    return has_latin ? std::string("auto") : configured;
}

bool idle_policy(int &seconds, bool &cache_enabled, std::string *error)
{
    seconds = 60;
    cache_enabled = true;
    wchar_t value[16]{};
    auto length = GetEnvironmentVariableW(L"HMI_TTS_IDLE_SECONDS", value, 16);
    if (length != 0)
    {
        int parsed = 0;
        bool valid = length < 16;
        for (DWORD index = 0; valid && index < length; ++index)
        {
            valid = value[index] >= L'0' && value[index] <= L'9' && parsed <= 360;
            if (valid) parsed = parsed * 10 + static_cast<int>(value[index] - L'0');
        }
        if (!valid || parsed > 3600)
        {
            if (error) *error = "HMI_TTS_IDLE_SECONDS must be 0..3600 (0 disables idle release)";
            return false;
        }
        seconds = parsed;
    }
    length = GetEnvironmentVariableW(L"HMI_TTS_PCM_CACHE", value, 16);
    if (length != 0)
    {
        if (length != 1 || (value[0] != L'0' && value[0] != L'1'))
        {
            if (error) *error = "HMI_TTS_PCM_CACHE must be 0 or 1";
            return false;
        }
        cache_enabled = value[0] == L'1';
    }
    return true;
}

bool sampling_policy(bool &enabled, std::string *error)
{
    wchar_t value[16]{};
    const auto length = GetEnvironmentVariableW(L"HMI_TTS_DO_SAMPLE", value, 16);
    if (length != 0 && (length != 1 || (value[0] != L'0' && value[0] != L'1')))
    {
        if (error) *error = "HMI_TTS_DO_SAMPLE must be 0 or 1";
        return false;
    }
    enabled = length == 0 || value[0] == L'1';
    return true;
}

int generation_budget(std::string_view text)
{
    using namespace detail::tts_segmentation;
    std::vector<Codepoint> points;
    if (!decode(text, points)) return 32;
    const auto count = static_cast<std::size_t>(std::count_if(points.begin(), points.end(), [](const Codepoint &point) {
        return spoken(point.value);
    }));
    // Bound codec generation, rather than trimming completed PCM or splitting
    // numeric/English content. This deliberately leaves room for natural pace.
    return static_cast<int>(std::clamp(std::size_t{24} + count * 6, std::size_t{32}, std::size_t{2048}));
}

std::uint64_t text_fingerprint(std::string_view text) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char value : text) { hash ^= static_cast<unsigned char>(value); hash *= 1099511628211ULL; }
    return hash;
}

std::uint16_t u16(const unsigned char *value)
{
    return static_cast<std::uint16_t>(static_cast<unsigned>(value[0]) |
                                      static_cast<unsigned>(value[1]) << 8);
}

std::uint32_t u32(const unsigned char *value)
{
    return static_cast<std::uint32_t>(value[0]) | static_cast<std::uint32_t>(value[1]) << 8 |
           static_cast<std::uint32_t>(value[2]) << 16 | static_cast<std::uint32_t>(value[3]) << 24;
}

bool read_reference(const std::string &path, audio::AudioBuffer &out, std::string *error)
{
    const std::filesystem::path native_path(std::u8string(path.begin(), path.end()));
    std::ifstream file(native_path, std::ios::binary | std::ios::ate);
    const std::streamoff length = file ? static_cast<std::streamoff>(file.tellg()) : -1;
    if (length < 12 || length > 64 * 1024 * 1024)
    {
        if (error) *error = "TTS reference WAV is missing or exceeds 64 MiB";
        return false;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
        std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    {
        if (error) *error = "TTS reference must be a RIFF/WAVE file";
        return false;
    }
    unsigned tag = 0, bits = 0, channels = 0;
    std::uint32_t rate = 0;
    const unsigned char *data = nullptr;
    std::size_t data_size = 0;
    for (std::size_t cursor = 12; cursor + 8 <= bytes.size();)
    {
        const std::size_t size = u32(bytes.data() + cursor + 4);
        cursor += 8;
        if (size > bytes.size() - cursor)
        {
            if (error) *error = "truncated TTS reference WAV chunk";
            return false;
        }
        if (std::memcmp(bytes.data() + cursor - 8, "fmt ", 4) == 0 && size >= 16)
        {
            tag = u16(bytes.data() + cursor);
            channels = u16(bytes.data() + cursor + 2);
            rate = u32(bytes.data() + cursor + 4);
            bits = u16(bytes.data() + cursor + 14);
            if (tag == WAVE_FORMAT_EXTENSIBLE && size >= 40)
                tag = u16(bytes.data() + cursor + 24);
        }
        else if (std::memcmp(bytes.data() + cursor - 8, "data", 4) == 0)
        {
            data = bytes.data() + cursor;
            data_size = size;
        }
        cursor += size;
        if ((size & 1U) != 0 && cursor < bytes.size()) ++cursor;
    }
    const std::size_t width = bits / 8;
    if (!data || data_size == 0 || channels < 1 || channels > 8 || rate < 8000 || rate > 192000 ||
        !((tag == WAVE_FORMAT_PCM && bits == 16) || (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32)) ||
        data_size % (static_cast<std::size_t>(channels) * width) != 0)
    {
        if (error) *error = "TTS reference requires PCM16 or float32 audio with a valid sample rate";
        return false;
    }
    out.samples.resize(data_size / width);
    out.sample_rate = static_cast<int>(rate);
    out.channels = static_cast<int>(channels);
    for (std::size_t index = 0; index < out.samples.size(); ++index)
    {
        if (tag == WAVE_FORMAT_PCM)
            out.samples[index] = static_cast<float>(static_cast<std::int16_t>(u16(data + index * 2))) / 32768.0f;
        else
            std::memcpy(&out.samples[index], data + index * 4, sizeof(float));
        if (!std::isfinite(out.samples[index]))
        {
            if (error) *error = "TTS reference contains non-finite PCM";
            out.samples.clear();
            return false;
        }
    }
    return true;
}
}

struct TtsSession::Impl
{
    using Clock = std::chrono::steady_clock;
    struct CachedPcm
    {
        std::string text;
        std::string context;
        audio::AudioBuffer audio;
    };
    HMODULE module = nullptr;
    audiocpp_registry *registry = nullptr;
    audiocpp_model *model = nullptr;
    audiocpp_session *session = nullptr;
    TtsConfig config;
    audio::AudioBuffer reference;
    bool reference_loaded = false;
    std::deque<CachedPcm> pcm_cache;
    std::size_t cache_bytes = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t idle_unloads = 0;
    int idle_timeout_seconds = 60;
    bool cache_enabled = true;
    bool sampling_enabled = true;
    Clock::time_point last_activity{};
    // Queue/model operations may take the statistics lock; snapshots take no
    // other lock and never wait for an SDK synthesis call.
    std::mutex queue_mutex;
    Clock::time_point idle_hold_deadline{};
    bool idle_release_in_progress = false;
    std::mutex synthesis_mutex;
    std::mutex statistics_mutex;
    TtsStatistics statistics;
    std::condition_variable condition;
    std::thread worker;
    std::string pending;
    std::string error;
    bool stopping = true;
    std::atomic<bool> opened{false};
    std::atomic<bool> working{false};
    std::atomic<bool> queued{false};
    std::atomic<std::uint64_t> generation{0};
    std::uint64_t active_generation = 0; // Protected by queue_mutex.
    bool prepare_pending = false; // Protected by queue_mutex; excluded from busy().
    bool preparing = false;
    std::uint64_t prepare_generation = 0;
    std::uint64_t prepare_request_id = 0;
    std::uint64_t pending_request_id = 0;
    Clock::time_point pending_at{}; // Protected by queue_mutex; replaced with pending speech.
    std::uint64_t active_request_id = 0;
    std::size_t active_segment_index = 0;
    std::string active_stop_reason; // Protected by queue_mutex.

    void trace(base::LogLevel level, std::string_view event, std::uint64_t request_id,
               std::size_t segment_index, std::string fields) const noexcept
    {
        if (!config.logger) return;
        try
        {
            config.logger->write(level, "tts_segment", "event=" + std::string(event) +
                " run_id=" + config.run_id +
                " request_id=" + std::to_string(request_id) + " segment_index=" +
                std::to_string(segment_index) + (fields.empty() ? std::string{} : " " + fields));
        }
        catch (...) { }
    }

    decltype(&::audiocpp_last_error) last_error = nullptr;
    decltype(&::audiocpp_model_load) model_load = nullptr;
    decltype(&::audiocpp_model_free) model_free = nullptr;
    decltype(&::audiocpp_session_create) session_create = nullptr;
    decltype(&::audiocpp_session_free) session_free = nullptr;
    decltype(&::audiocpp_request_create) request_create = nullptr;
    decltype(&::audiocpp_request_free) request_free = nullptr;
    decltype(&::audiocpp_request_set_text) request_set_text = nullptr;
    decltype(&::audiocpp_request_set_option) request_set_option = nullptr;
    decltype(&::audiocpp_request_set_voice_id) request_set_voice_id = nullptr;
    decltype(&::audiocpp_request_set_voice_audio) request_set_voice_audio = nullptr;
    decltype(&::audiocpp_session_run) session_run = nullptr;
    decltype(&::audiocpp_result_audio) result_audio = nullptr;
    decltype(&::audiocpp_result_free) result_free = nullptr;
    decltype(&::audiocpp_result_artifact_count) result_artifact_count = nullptr;
    decltype(&::audiocpp_result_artifact) result_artifact = nullptr;
    decltype(&::audiocpp_result_artifact_meta_count) result_artifact_meta_count = nullptr;
    decltype(&::audiocpp_result_artifact_meta) result_artifact_meta = nullptr;

    bool bind(std::string *out_error)
    {
        const auto load = [this, out_error](const char *name, auto &target) {
            target = reinterpret_cast<std::decay_t<decltype(target)>>(GetProcAddress(module, name));
            if (!target && out_error) *out_error = "native TTS interface missing: " + detail::native_diagnostic(name);
            return target != nullptr;
        };
#define HMI_TTS_BIND(name) if (!load("audiocpp_" #name, name)) return false
        HMI_TTS_BIND(last_error); HMI_TTS_BIND(model_load); HMI_TTS_BIND(model_free);
        HMI_TTS_BIND(session_create); HMI_TTS_BIND(session_free);
        HMI_TTS_BIND(request_create); HMI_TTS_BIND(request_free); HMI_TTS_BIND(request_set_text);
        HMI_TTS_BIND(request_set_option);
        HMI_TTS_BIND(request_set_voice_id); HMI_TTS_BIND(request_set_voice_audio);
        HMI_TTS_BIND(session_run); HMI_TTS_BIND(result_audio); HMI_TTS_BIND(result_free);
#undef HMI_TTS_BIND
        const auto optional = [this](const char *name, auto &target) {
            target = reinterpret_cast<std::decay_t<decltype(target)>>(GetProcAddress(module, name));
        };
        optional("audiocpp_result_artifact_count", result_artifact_count);
        optional("audiocpp_result_artifact", result_artifact);
        optional("audiocpp_result_artifact_meta_count", result_artifact_meta_count);
        optional("audiocpp_result_artifact_meta", result_artifact_meta);
        const bool any_artifact_api = result_artifact_count || result_artifact || result_artifact_meta_count || result_artifact_meta;
        const bool complete_artifact_api = result_artifact_count && result_artifact && result_artifact_meta_count && result_artifact_meta;
        if (any_artifact_api && !complete_artifact_api)
        {
            if (out_error) *out_error = "native TTS artifact interface is incomplete";
            return false;
        }
        return true;
    }

    bool check(audiocpp_status status, const char *operation, std::string *out_error) const
    {
        if (status == AUDIOCPP_OK) return true;
        if (out_error)
        {
            const char *detail = last_error ? last_error() : nullptr;
            *out_error = std::string(operation) + ": " +
                (detail ? hmi::engine::detail::native_diagnostic(detail) : "engine call failed");
        }
        return false;
    }

    void release_model(bool reset_statistics = true)
    {
        if (session && session_free) session_free(session);
        if (model && model_free) model_free(model);
        session = nullptr;
        model = nullptr;
        reference = {};
        reference_loaded = false;
        last_activity = {};
        std::lock_guard lock(statistics_mutex);
        if (reset_statistics)
        {
            statistics = {};
            statistics.idle_timeout_seconds = idle_timeout_seconds;
            statistics.idle_unloads = idle_unloads;
            statistics.cache_entries = pcm_cache.size();
            statistics.cache_bytes = cache_bytes;
            statistics.cache_hits = cache_hits;
            statistics.sampling_enabled = sampling_enabled;
        }
        else
        {
            statistics.model_ready = false;
            statistics.backend.clear();
            statistics.idle_unloads = idle_unloads;
        }
    }

    void clear_speech_statistics(std::uint64_t request_id = 0)
    {
        std::lock_guard lock(statistics_mutex);
        statistics.request_id = request_id;
        statistics.segments = statistics.generated_segments = statistics.queue_high_water = 0;
        statistics.generated_audio_frames = 0;
        statistics.first_audio_ms = statistics.synthesis_ms = statistics.total_ms = 0.0;
        statistics.queue_ms = 0.0;
        statistics.audio_seconds = 0.0;
        statistics.generation_retries = statistics.limit_hits = 0;
        statistics.last_finish_reason.clear();
        statistics.pipeline_finish_reason.clear();
        statistics.model_load_ms = 0.0;
        statistics.last_request_cache_hits = 0;
        statistics.utterance_cache_hits = 0;
        statistics.recovery_splits = statistics.recovery_attempts = 0;
        statistics.recovery_exhausted = false;
        statistics.sampling_fallbacks = 0;
        statistics.max_tokens = 0;
        statistics.producer_wait_ms = statistics.consumer_wait_ms = 0.0;
        statistics.consumer_pending_reads = statistics.consumed_chunks = 0;
        statistics.playback = {};
    }

    std::string voice_context(std::string_view language) const
    {
        std::string context;
        const auto append = [&context](std::string_view value) {
            context += std::to_string(value.size());
            context.push_back(':');
            context.append(value);
        };
        // Exact length framing, rather than a hash, determines cache equality.
        for (const std::string_view value : {std::string_view(config.model_path), language,
                 std::string_view(config.voice_id), std::string_view(config.reference_wav),
                 std::string_view(config.instruction), sampling_enabled ? std::string_view("sample") :
                 std::string_view("greedy"), std::string_view("42"), std::string_view("0.9"),
                 std::string_view("50"), std::string_view("1.0"), std::string_view("1.05"),
                 std::string_view("0.9"), std::string_view("50"), std::string_view("1.0")}) append(value);
        return context;
    }

    std::string voice_fields(std::string_view language, std::string_view context) const
    {
        return " language=" + std::string(language) + " voice_id=" + config.voice_id +
            " reference_audio=" + (config.reference_wav.empty() ? "0" : "1") +
            " do_sample=" + (sampling_enabled ? "1" : "0") +
            " subtalker_do_sample=" + (sampling_enabled ? "1" : "0") +
            " seed=42 instruction_hash=" + std::to_string(text_fingerprint(config.instruction)) +
            " policy_hash=" + std::to_string(text_fingerprint(context));
    }

    bool cached_audio(std::string_view text, std::string_view context, audio::AudioBuffer &audio)
    {
        if (!cache_enabled) return false;
        const auto found = std::find_if(pcm_cache.begin(), pcm_cache.end(), [text, context](const CachedPcm &entry) {
            return entry.text == text && entry.context == context;
        });
        if (found == pcm_cache.end()) return false;
        audio = found->audio;
        std::rotate(pcm_cache.begin(), found, found + 1);
        std::lock_guard lock(statistics_mutex);
        statistics.last_request_cache_hits = 1;
        ++statistics.utterance_cache_hits;
        statistics.cache_hits = ++cache_hits;
        return true;
    }

    void remember_audio(std::string_view text, std::string_view context, const audio::AudioBuffer &audio)
    {
        constexpr std::size_t maximum_entry_bytes = 512 * 1024;
        constexpr std::size_t maximum_cache_bytes = 2 * 1024 * 1024;
        const auto bytes = audio.samples.size() * sizeof(float);
        if (!cache_enabled || bytes > maximum_entry_bytes) return;
        pcm_cache.push_front({std::string(text), std::string(context), audio});
        cache_bytes += bytes;
        while (pcm_cache.size() > 4 || cache_bytes > maximum_cache_bytes)
        {
            cache_bytes -= pcm_cache.back().audio.samples.size() * sizeof(float);
            pcm_cache.pop_back();
        }
        std::lock_guard lock(statistics_mutex);
        statistics.cache_entries = pcm_cache.size();
        statistics.cache_bytes = cache_bytes;
    }

    void release_if_idle()
    {
        // Never hold the queue while waiting for SDK synthesis or playback.
        std::unique_lock synthesis_lock(synthesis_mutex, std::try_to_lock);
        if (!synthesis_lock || !session || idle_timeout_seconds == 0 ||
            last_activity == Clock::time_point{} ||
            Clock::now() - last_activity < std::chrono::seconds(idle_timeout_seconds)) return;
        {
            std::lock_guard queue_lock(queue_mutex);
            if (stopping || !pending.empty() || working.load() || prepare_pending || preparing ||
                Clock::now() < idle_hold_deadline) return;
            idle_release_in_progress = true;
        }
        struct ReleaseMarker
        {
            Impl &impl;
            ~ReleaseMarker()
            {
                std::lock_guard queue_lock(impl.queue_mutex);
                impl.idle_release_in_progress = false;
                impl.idle_hold_deadline = {};
            }
        } marker{*this};
        ++idle_unloads;
        release_model(false);
    }

    bool load_model(std::uint64_t request_generation, std::string *out_error, bool speech_statistics = true)
    {
        if (session) return true;
        const auto started = Clock::now();
        // Opt-in until latency and output quality have been checked on this GPU.
        wchar_t requested[32]{};
        const auto length = GetEnvironmentVariableW(L"HMI_TTS_PERF_MODE", requested, 32);
        const std::wstring performance = length < 32 ? requested : L"invalid";
        if (!performance.empty() && performance != L"off" && performance != L"flash_attention" &&
            performance != L"cuda_perf")
        {
            if (out_error) *out_error = "HMI_TTS_PERF_MODE must be off, flash_attention, or cuda_perf";
            return false;
        }
        using OptionsFree = decltype(&::audiocpp_options_free);
        std::unique_ptr<audiocpp_options, OptionsFree> options(nullptr, nullptr);
        if (!performance.empty() && performance != L"off" &&
            (config.backend == "auto" || config.backend == "cuda"))
        {
            const auto create = reinterpret_cast<decltype(&::audiocpp_options_create)>(
                GetProcAddress(module, "audiocpp_options_create"));
            const auto set = reinterpret_cast<decltype(&::audiocpp_options_set)>(
                GetProcAddress(module, "audiocpp_options_set"));
            const auto release = reinterpret_cast<OptionsFree>(GetProcAddress(module, "audiocpp_options_free"));
            if (!create || !set || !release)
            {
                if (out_error) *out_error = "native TTS performance options interface missing";
                return false;
            }
            options = decltype(options)(create(), release);
            if (!options)
            {
                if (out_error) *out_error = "cannot allocate native TTS performance options";
                return false;
            }
            const char *mode = performance == L"cuda_perf" ? "cuda_perf" : "flash_attention";
            if (!check(set(options.get(), "qwen3_tts.perf_mode", mode),
                       "set TTS performance mode", out_error)) return false;
        }
        const audiocpp_model_config selection{"qwen3_tts", nullptr, nullptr, nullptr};
        std::string selected_backend;
        if (!check(model_load(registry, config.model_path.c_str(), &selection, nullptr, &model),
                   "load Qwen3-TTS model", out_error) ||
            !detail::create_sdk_session({session_create, session_free, last_error},
                model, "tts", "offline", config.backend, config.threads, &session, out_error,
                &selected_backend, options.get()))
        {
            release_model(false);
            return false;
        }
        {
            std::lock_guard queue_lock(queue_mutex);
            std::lock_guard lock(statistics_mutex);
            statistics.model_ready = true;
            statistics.backend = std::move(selected_backend);
            statistics.perf_mode = statistics.backend != "cuda" ? "off" :
                performance == L"cuda_perf" ? "cuda_perf" :
                performance == L"flash_attention" ? "flash_attention" : "off";
            if (speech_statistics && generation.load() == request_generation)
                statistics.model_load_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        }
        return true;
    }

    void run_preparation(std::uint64_t current, std::uint64_t request_id)
    {
        const auto started = Clock::now();
        trace(base::LogLevel::Info, "prepare_started", request_id, 0,
            "requested_backend=" + config.backend);
        std::string detail;
        bool ready = false;
        bool superseded = false;
        {
            std::lock_guard synthesis_lock(synthesis_mutex);
            {
                std::lock_guard queue_lock(queue_mutex);
                superseded = stopping || !opened.load() || generation.load() != current;
            }
            if (!superseded)
            {
                try
                {
                    const hmi::engine::detail::ScopedSdkThreads sdk_threads(config.threads);
                    ready = load_model(current, &detail, false);
                }
                catch (const std::exception &exception)
                {
                    detail = exception.what();
                    release_model(false);
                }
                catch (...)
                {
                    detail = "unexpected TTS model preparation failure";
                    release_model(false);
                }
            }
            if (session) last_activity = Clock::now();
        }
        std::string backend;
        bool model_ready = false;
        {
            std::lock_guard queue_lock(queue_mutex);
            superseded = superseded || stopping || !opened.load() || generation.load() != current;
            preparing = false;
            std::lock_guard statistics_lock(statistics_mutex);
            model_ready = statistics.model_ready;
            backend = statistics.backend;
        }
        trace(!ready && !superseded ? base::LogLevel::Warning : base::LogLevel::Info,
            "prepare_finished", request_id, 0,
            std::string("result=") + (superseded ? "superseded" : ready ? "ready" : "failed") +
            " model_ready=" + (model_ready ? "1" : "0") + " backend=" + backend +
            " wall_ms=" + std::to_string(std::chrono::duration<double, std::milli>(Clock::now() - started).count()) +
            (detail.empty() || superseded ? std::string{} : " detail=" + detail));
        condition.notify_one();
    }

    void run(TtsSession *owner)
    {
        for (;;)
        {
            std::string text;
            std::uint64_t current = 0;
            std::uint64_t request_id = 0;
            double worker_queue_ms = 0.0;
            bool prepare_only = false;
            {
                std::unique_lock lock(queue_mutex);
                if (!condition.wait_for(lock, std::chrono::seconds(1), [this] {
                        return stopping || !pending.empty() || prepare_pending;
                    }))
                {
                    lock.unlock();
                    release_if_idle();
                    continue;
                }
                if (stopping) return;
                if (pending.empty())
                {
                    prepare_only = true;
                    preparing = true;
                    prepare_pending = false;
                    current = prepare_generation = generation.load();
                    request_id = prepare_request_id;
                }
                else
                {
                    text = std::move(pending);
                    pending.clear();
                    current = generation.load();
                    active_generation = current;
                    request_id = active_request_id = pending_request_id;
                    worker_queue_ms = std::chrono::duration<double, std::milli>(Clock::now() - pending_at).count();
                    pending_at = {};
                    active_segment_index = 0;
                    active_stop_reason.clear();
                    working.store(true);
                    queued.store(false);
                }
            }
            if (prepare_only)
            {
                run_preparation(current, request_id);
                continue;
            }
            std::string detail;
            bool ok = false;
            hmi::engine::detail::TtsPipelineStatistics measured;
            audio::PlaybackStatistics playback;
            std::string segment_finish_reason;
            try
            {
                const auto language = utterance_language(text, config.language);
                const auto context = voice_context(language);
                ok = hmi::engine::detail::run_tts_pipeline(text,
                    [this, owner, current, request_id, &segment_finish_reason, &language, &context](std::string_view segment, audio::AudioBuffer &audio,
                                                      std::string *out_error) {
                        std::size_t index = 0;
                        {
                            std::lock_guard lock(queue_mutex);
                            if (generation.load() == current) index = ++active_segment_index;
                        }
                        const auto started = Clock::now();
                        const bool accepted = owner->synthesize_internal(segment, audio, out_error, true, current, language);
                        const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                        const auto state = owner->statistics();
                        const bool superseded = generation.load() != current || !opened.load();
                        const bool matching = !superseded && state.request_id == request_id;
                        segment_finish_reason = matching ? state.last_finish_reason : std::string{};
                        trace(accepted || superseded ? base::LogLevel::Info : base::LogLevel::Warning,
                            "segment_finished", request_id, index,
                            "result=" + std::string(superseded ? "superseded" : accepted ? "complete" : "failed") +
                            " text_bytes=" + std::to_string(segment.size()) +
                            " text_hash=" + std::to_string(text_fingerprint(segment)) +
                            voice_fields(language, context) +
                            " wall_ms=" + std::to_string(wall_ms) + " sample_rate=" +
                            std::to_string(accepted ? audio.sample_rate : 0) +
                            " channels=" + std::to_string(accepted ? audio.channels : 0) + " pcm_frames=" +
                            std::to_string(audio.channels > 0 ? audio.samples.size() / static_cast<std::size_t>(audio.channels) : 0) +
                            (matching ? " finish_reason=" + state.last_finish_reason +
                                " sdk_budget=" + std::to_string(state.last_request_cache_hits ? 0 : state.max_tokens) +
                                " cache_hit=" + std::to_string(state.last_request_cache_hits) : " statistics=superseded") +
                            (accepted || !out_error ? std::string{} : " detail=" + *out_error));
                        return accepted;
                    },
                    [this, &playback](const audio::detail::ReadAudio &read, std::string *out_error,
                                const std::function<bool()> &cancelled) {
                        if (config.playback) return config.playback(read, out_error, cancelled);
                        return audio::detail::play_stream(read, out_error, cancelled, &playback);
                    },
                    [this, current] { return !opened.load() || generation.load() != current; }, &detail, &measured,
                    [this, current, request_id, worker_queue_ms](const hmi::engine::detail::TtsPipelineStatistics &progress) {
                        std::lock_guard lock(queue_mutex);
                        if (generation.load() != current) return;
                        std::lock_guard statistics_lock(statistics_mutex);
                        statistics.segments = progress.segments;
                        statistics.generated_segments = progress.generated_segments;
                        statistics.generated_audio_frames = progress.generated_audio_frames;
                        statistics.queue_high_water = progress.queue_high_water;
                        statistics.queue_ms = worker_queue_ms;
                        statistics.first_audio_ms = progress.generated_segments ? worker_queue_ms + progress.first_audio_ms : 0.0;
                        statistics.synthesis_ms = progress.synthesis_ms;
                        statistics.total_ms = worker_queue_ms + progress.total_ms;
                        statistics.audio_seconds = progress.audio_seconds;
                        statistics.producer_wait_ms = progress.producer_wait_ms;
                        statistics.consumer_wait_ms = progress.consumer_wait_ms;
                        statistics.consumer_pending_reads = progress.consumer_pending_reads;
                        statistics.consumed_chunks = progress.consumed_chunks;
                        if (progress.recovery_splits > statistics.recovery_splits)
                            trace(base::LogLevel::Info, "recovery_split", request_id, 0,
                                "splits=" + std::to_string(progress.recovery_splits) +
                                " attempts=" + std::to_string(progress.recovery_attempts));
                        statistics.recovery_splits = progress.recovery_splits;
                        statistics.recovery_attempts = progress.recovery_attempts;
                        statistics.recovery_exhausted = progress.recovery_exhausted;
                        statistics.pipeline_finish_reason = progress.finish_reason;
                    },
                    [this, current, request_id, &segment_finish_reason](std::string_view segment) {
                        std::size_t index = 0;
                        {
                            std::lock_guard lock(queue_mutex);
                            if (generation.load() != current || !opened.load() ||
                                (segment_finish_reason != "length" && segment_finish_reason != "length_heuristic"))
                                return hmi::engine::detail::SegmentFailureReason::Other;
                            index = active_segment_index;
                        }
                        trace(base::LogLevel::Info, "recovery_requested", request_id, index,
                            "text_bytes=" + std::to_string(segment.size()) +
                            " text_hash=" + std::to_string(text_fingerprint(segment)));
                        return hmi::engine::detail::SegmentFailureReason::LengthLimit;
                    });
            }
            catch (const std::exception &exception)
            {
                detail = exception.what();
            }
            catch (...)
            {
                detail = "unexpected TTS worker failure";
            }
            measured.first_audio_ms = measured.generated_segments ? worker_queue_ms + measured.first_audio_ms : 0.0;
            measured.total_ms += worker_queue_ms;
            if (!ok && measured.finish_reason != "cancelled" && !measured.finish_reason.empty())
                playback.finish_reason = measured.finish_reason;
            if (measured.recovery_splits != 0 || measured.recovery_exhausted)
                trace(measured.recovery_exhausted ? base::LogLevel::Warning : base::LogLevel::Info,
                    measured.recovery_exhausted ? "recovery_exhausted" : "recovery_finished", request_id, 0,
                    "splits=" + std::to_string(measured.recovery_splits) +
                    " attempts=" + std::to_string(measured.recovery_attempts) +
                    " finish_reason=" + measured.finish_reason);
            {
                std::lock_guard synthesis_lock(synthesis_mutex);
                if (session) last_activity = Clock::now();
            }
            const std::size_t sampling_fallbacks = 0;
            bool publish_acknowledgement = false;
            bool failed = false;
            std::string reason;
            {
                std::lock_guard lock(queue_mutex);
                const bool superseded = generation.load() != current;
                publish_acknowledgement = superseded || !ok;
                failed = !ok && !superseded && measured.finish_reason != "cancelled";
                reason = active_stop_reason;
                if (reason.empty()) reason = measured.finish_reason.empty() ?
                    (!ok ? "generation_or_playback_failed" : "completed") : measured.finish_reason;
                if (!superseded)
                {
                    error = ok ? std::string{} : detail;
                    std::lock_guard statistics_lock(statistics_mutex);
                    statistics.segments = measured.segments;
                    statistics.generated_segments = measured.generated_segments;
                    statistics.generated_audio_frames = measured.generated_audio_frames;
                    statistics.queue_high_water = measured.queue_high_water;
                    statistics.queue_ms = worker_queue_ms;
                    statistics.first_audio_ms = measured.first_audio_ms;
                    statistics.synthesis_ms = measured.synthesis_ms;
                    statistics.total_ms = measured.total_ms;
                    statistics.audio_seconds = measured.audio_seconds;
                    statistics.producer_wait_ms = measured.producer_wait_ms;
                    statistics.consumer_wait_ms = measured.consumer_wait_ms;
                    statistics.consumer_pending_reads = measured.consumer_pending_reads;
                    statistics.consumed_chunks = measured.consumed_chunks;
                    statistics.playback = playback;
                    statistics.recovery_splits = measured.recovery_splits;
                    statistics.recovery_attempts = measured.recovery_attempts;
                    statistics.recovery_exhausted = measured.recovery_exhausted;
                    statistics.pipeline_finish_reason = measured.finish_reason;
                }
                working.store(false);
            }
            // Commit the final snapshot or choose its detached acknowledgement atomically.
            // A later cancel then sees finalized statistics; normal completion adds no duplicate record.
            if (config.logger && publish_acknowledgement)
            {
                try
                {
                    using util::json::Value;
                    config.logger->write(failed ? base::LogLevel::Error : base::LogLevel::Info, "tts", "event=playback_result " +
                        util::json::stringify(Value(Value::Object{
                            {"event", Value("playback_result")}, {"schema", Value(1.0)},
                            {"run_id", Value(config.run_id)}, {"request_id", Value(std::to_string(request_id))},
                            {"stop_reason", Value(std::move(reason))}, {"request_finished", Value(true)},
                            {"statistics_final", Value(true)}, {"playback_final", Value(playback.finalized)},
                            {"tts_final", Value(Value::Object{
                                {"queue_ms", Value(worker_queue_ms)},
                                {"synthesis_ms", Value(measured.synthesis_ms)}, {"total_ms", Value(measured.total_ms)},
                                {"segments", Value(static_cast<double>(measured.segments))},
                                {"generated_segments", Value(static_cast<double>(measured.generated_segments))},
                                {"generated_audio_frames", Value(static_cast<double>(measured.generated_audio_frames))},
                                {"audio_seconds", Value(measured.audio_seconds)},
                                {"first_pcm_ms", measured.generated_segments ? Value(measured.first_audio_ms) : Value{}},
                                {"first_pcm_origin", Value("speak_accepted")},
                                {"playback_timing_origin", Value("play_stream_started")},
                                {"producer_wait_ms", Value(measured.producer_wait_ms)},
                                {"consumer_wait_ms", Value(measured.consumer_wait_ms)},
                                {"recovery_splits", Value(static_cast<double>(measured.recovery_splits))},
                                {"recovery_attempts", Value(static_cast<double>(measured.recovery_attempts))},
                                {"recovery_exhausted", Value(measured.recovery_exhausted)},
                                {"sampling_fallbacks", Value(static_cast<double>(sampling_fallbacks))},
                                {"finish_reason", Value(measured.finish_reason)}})},
                            {"playback", audio::detail::playback_fields(playback)}})));
                }
                catch (...) { }
            }
        }
    }
};

TtsSession::TtsSession() : impl_(std::make_unique<Impl>()) {}
TtsSession::~TtsSession() { stop(); }

bool TtsSession::open(Client &client, const TtsConfig &config, std::string *error)
{
    stop();
    if (error) error->clear();
    if (!client.ready() || config.model_path.empty() || config.threads < 1)
    {
        if (error) *error = "TTS requires an open engine and a valid model configuration";
        return false;
    }
    if (!config.instruction.empty() && !valid_text(config.instruction))
    {
        if (error) *error = "TTS instruction must be valid UTF-8 text within 4096 bytes";
        return false;
    }
    const auto directory = base::process_directory();
    std::filesystem::path model_path;
    if (!base::resolve_project_path(std::filesystem::path(std::u8string(config.model_path.begin(),
            config.model_path.end())), directory, model_path, error)) return false;
    TtsConfig selected = config;
    const auto model_utf8 = model_path.generic_u8string();
    selected.model_path.assign(model_utf8.begin(), model_utf8.end());
    if (!config.reference_wav.empty())
    {
        std::filesystem::path reference_path;
        if (!base::resolve_project_path(std::filesystem::path(std::u8string(config.reference_wav.begin(),
                config.reference_wav.end())), directory, reference_path, error)) return false;
        const auto reference_utf8 = reference_path.generic_u8string();
        selected.reference_wav.assign(reference_utf8.begin(), reference_utf8.end());
    }
    Impl &impl = *impl_;
    int idle_seconds = 60;
    bool cache_enabled = true;
    if (!idle_policy(idle_seconds, cache_enabled, error)) return false;
    bool sampling_enabled = false;
    if (!sampling_policy(sampling_enabled, error)) return false;
    impl.module = detail::retain_sdk_library(client.module_handle());
    if (!impl.module)
    {
        if (error) *error = "cannot retain the loaded native engine for TTS";
        return false;
    }
    if (!impl.bind(error))
    {
        FreeLibrary(impl.module);
        impl.module = nullptr;
        return false;
    }
    impl.registry = client.registry();
    impl.config = std::move(selected);
    impl.idle_timeout_seconds = idle_seconds;
    impl.cache_enabled = cache_enabled;
    impl.sampling_enabled = sampling_enabled;
    {
        std::lock_guard lock(impl.statistics_mutex);
        impl.statistics.idle_timeout_seconds = idle_seconds;
        impl.statistics.sampling_enabled = sampling_enabled;
    }
    {
        std::lock_guard lock(impl.queue_mutex);
        impl.stopping = false;
        impl.error.clear();
    }
    impl.opened.store(true);
    try { impl.worker = std::thread([this] { impl_->run(this); }); }
    catch (const std::exception &exception)
    {
        if (error) *error = exception.what();
        stop();
        return false;
    }
    return true;
}

bool TtsSession::prepare(std::uint64_t request_id)
{
    Impl &impl = *impl_;
    {
        std::lock_guard lock(impl.queue_mutex);
        if (impl.stopping || !impl.opened.load()) return false;
        if (impl.prepare_pending || (impl.preparing && impl.prepare_generation == impl.generation.load())) return true;
        if (!impl.idle_release_in_progress)
        {
            std::lock_guard statistics_lock(impl.statistics_mutex);
            if (impl.statistics.model_ready) return true;
        }
        impl.prepare_pending = true;
        impl.prepare_request_id = request_id;
    }
    impl.condition.notify_one();
    return true;
}

bool TtsSession::speak(std::string_view text)
{
    return speak(text, 0);
}

bool TtsSession::speak(std::string_view text, std::uint64_t request_id)
{
    Impl &impl = *impl_;
    if (!valid_text(text))
    {
        std::lock_guard lock(impl.queue_mutex);
        impl.error = "TTS requires non-empty UTF-8 text, at most 4096 bytes";
        return false;
    }
    {
        std::lock_guard lock(impl.queue_mutex);
        if (impl.stopping || !impl.opened.load())
        {
            impl.error = "TTS is not open";
            return false;
        }
        impl.pending = text;
        impl.prepare_pending = false;
        if (impl.working.load() && impl.active_stop_reason.empty()) impl.active_stop_reason = "request_replaced";
        impl.pending_request_id = request_id;
        impl.pending_at = Impl::Clock::now();
        impl.idle_hold_deadline = {};
        impl.error.clear();
        impl.queued.store(true);
        impl.generation.fetch_add(1);
        impl.clear_speech_statistics(request_id);
    }
    impl.condition.notify_one();
    return true;
}

void TtsSession::cancel(std::string_view reason)
{
    Impl &impl = *impl_;
    {
        std::lock_guard lock(impl.queue_mutex);
        impl.pending.clear();
        impl.pending_at = {};
        impl.prepare_pending = false;
        if (impl.working.load() && impl.active_stop_reason.empty())
            impl.active_stop_reason = reason.empty() ? "cancelled" : std::string(reason);
        impl.queued.store(false);
        impl.generation.fetch_add(1);
        impl.error.clear();
    }
    impl.condition.notify_all();
}

bool TtsSession::defer_idle_unload(int seconds)
{
    if (seconds < 0 || seconds > 45) return false;
    Impl &impl = *impl_;
    {
        std::lock_guard lock(impl.queue_mutex);
        if (seconds == 0)
            impl.idle_hold_deadline = {};
        else
        {
            if (impl.stopping || !impl.opened.load() || impl.idle_release_in_progress) return false;
            std::lock_guard statistics_lock(impl.statistics_mutex);
            if (!impl.statistics.model_ready) return false;
            impl.idle_hold_deadline = Impl::Clock::now() + std::chrono::seconds(seconds);
        }
    }
    impl.condition.notify_one();
    return true;
}

void TtsSession::stop()
{
    Impl &impl = *impl_;
    {
        std::lock_guard lock(impl.queue_mutex);
        impl.stopping = true;
        if (impl.working.load() && impl.active_stop_reason.empty()) impl.active_stop_reason = "runtime_stopped";
        impl.opened.store(false);
        impl.pending.clear();
        impl.pending_at = {};
        impl.prepare_pending = false;
        impl.idle_hold_deadline = {};
        impl.idle_release_in_progress = false;
        impl.queued.store(false);
        impl.generation.fetch_add(1);
    }
    impl.condition.notify_all();
    if (impl.worker.joinable()) impl.worker.join();
    std::lock_guard lock(impl.synthesis_mutex);
    impl.pcm_cache.clear();
    impl.cache_bytes = 0;
    impl.cache_hits = impl.idle_unloads = 0;
    impl.sampling_enabled = false;
    impl.release_model();
    if (impl.module) FreeLibrary(impl.module);
    impl.module = nullptr;
    impl.registry = nullptr;
    impl.working.store(false);
    {
        std::lock_guard queue_lock(impl.queue_mutex);
        impl.preparing = false;
        impl.prepare_generation = impl.prepare_request_id = 0;
    }
}

bool TtsSession::busy() const noexcept
{
    std::lock_guard lock(impl_->queue_mutex);
    return impl_->working.load() || impl_->queued.load();
}

std::string TtsSession::last_error() const
{
    std::lock_guard lock(impl_->queue_mutex);
    return impl_->error;
}

TtsStatistics TtsSession::statistics() const
{
    std::lock_guard lock(impl_->statistics_mutex);
    return impl_->statistics;
}

bool TtsSession::synthesize(std::string_view text, audio::AudioBuffer &out, std::string *error)
{
    return synthesize_internal(text, out, error, false, 0);
}

bool TtsSession::synthesize_internal(std::string_view text, audio::AudioBuffer &out, std::string *error,
                                    bool worker_request, std::uint64_t worker_generation,
                                    std::string_view requested_language)
{
    out = {};
    if (error) error->clear();
    Impl &impl = *impl_;
    std::unique_lock synthesis_lock(impl.synthesis_mutex, std::defer_lock);
    std::uint64_t current_generation = 0;
    std::uint64_t request_id = 0;
    std::size_t segment_index = 0;
    if (worker_request)
    {
        synthesis_lock.lock();
        std::lock_guard queue_lock(impl.queue_mutex);
        current_generation = worker_generation;
        request_id = impl.active_request_id;
        segment_index = impl.active_segment_index;
    }
    else
    {
        std::lock_guard queue_lock(impl.queue_mutex);
        if (impl.working.load() || impl.queued.load() || impl.prepare_pending || impl.preparing ||
            !synthesis_lock.try_lock())
        {
            if (error) *error = "TTS is busy; synchronous synthesis was not started";
            return false;
        }
        current_generation = impl.generation.load();
    }
    const auto cancelled = [&] { return !impl.opened.load() || impl.generation.load() != current_generation; };
    if (!impl.opened.load() || !valid_text(text))
    {
        if (error) *error = "TTS is not open or text is invalid";
        return false;
    }
    const auto language = requested_language.empty() ? utterance_language(text, impl.config.language) :
        std::string(requested_language);
    const auto context = impl.voice_context(language);
    const auto voice_fields = impl.voice_fields(language, context);
    struct Activity
    {
        Impl &impl;
        ~Activity()
        {
            if (impl.session) impl.last_activity = Impl::Clock::now();
            impl.condition.notify_one();
        }
    } activity{impl};
    bool cache_hit = false;
    {
        std::lock_guard queue_lock(impl.queue_mutex);
        if (cancelled())
        {
            if (error) *error = "TTS synthesis cancelled";
            return false;
        }
        if (!worker_request)
            impl.clear_speech_statistics();
        else
        {
            std::lock_guard statistics_lock(impl.statistics_mutex);
            impl.statistics.last_request_cache_hits = 0;
            impl.statistics.last_finish_reason.clear();
        }
        if (impl.cached_audio(text, context, out))
        {
            std::lock_guard statistics_lock(impl.statistics_mutex);
            impl.statistics.last_finish_reason = "cache";
            if (!worker_request)
            {
                impl.statistics.generated_audio_frames = out.samples.size() / static_cast<std::size_t>(out.channels);
                impl.statistics.audio_seconds = static_cast<double>(impl.statistics.generated_audio_frames) / out.sample_rate;
            }
            cache_hit = true;
        }
    }
    if (cache_hit)
    {
        if (cancelled())
        {
            out = {};
            if (error) *error = "TTS synthesis cancelled";
            return false;
        }
        impl.trace(base::LogLevel::Info, "pcm_cache_hit", request_id, segment_index,
            "text_bytes=" + std::to_string(text.size()) + " text_hash=" + std::to_string(text_fingerprint(text)) +
            voice_fields + " sample_rate=" + std::to_string(out.sample_rate) +
            " channels=" + std::to_string(out.channels) + " pcm_frames=" +
            std::to_string(out.samples.size() / static_cast<std::size_t>(out.channels)));
        return true;
    }
    const detail::ScopedSdkThreads sdk_threads(impl.config.threads);
    if (!impl.load_model(current_generation, error)) return false;
    audiocpp_request *request = impl.request_create();
    if (!request)
    {
        if (error) *error = "cannot allocate TTS request";
        return false;
    }
    const std::unique_ptr<audiocpp_request, decltype(impl.request_free)> owned_request(request, impl.request_free);
    const std::string owned_text(text);
    bool ok = impl.check(impl.request_set_text(request, owned_text.c_str(), language.c_str()),
                          "set TTS text", error);
    int max_tokens = generation_budget(text);
    if (ok && request_id != 0)
    {
        const auto request_key = std::to_string(request_id);
        const auto segment_key = std::to_string(segment_index);
        ok = impl.check(impl.request_set_option(request, "hmi.request_id", request_key.c_str()), "set TTS request trace", error) &&
             impl.check(impl.request_set_option(request, "hmi.segment_index", segment_key.c_str()), "set TTS segment trace", error);
    }
    const char *sampling = impl.sampling_enabled ? "true" : "false";
    const std::pair<const char *, const char *> generation_options[]{
        {"do_sample", sampling}, {"subtalker_do_sample", sampling}, {"seed", "42"},
        {"temperature", "0.9"}, {"top_k", "50"}, {"top_p", "1.0"},
        {"repetition_penalty", "1.05"}, {"subtalker_temperature", "0.9"},
        {"subtalker_top_k", "50"}, {"subtalker_top_p", "1.0"}};
    for (const auto &[key, value] : generation_options)
    {
        if (!ok) break;
        ok = impl.check(impl.request_set_option(request, key, value), "set TTS generation policy", error);
    }
    if (ok && !impl.config.instruction.empty())
        ok = impl.check(impl.request_set_option(request, "instruction", impl.config.instruction.c_str()),
                        "set TTS voice style", error);
    if (ok && !impl.config.reference_wav.empty())
    {
        if (!impl.reference_loaded)
        {
            ok = read_reference(impl.config.reference_wav, impl.reference, error);
            impl.reference_loaded = ok;
        }
        if (ok)
            ok = impl.check(impl.request_set_voice_audio(request, impl.reference.samples.data(),
                                impl.reference.samples.size() / static_cast<std::size_t>(impl.reference.channels),
                                impl.reference.sample_rate, impl.reference.channels), "set TTS reference audio", error);
    }
    else if (ok && !impl.config.voice_id.empty())
        ok = impl.check(impl.request_set_voice_id(request, impl.config.voice_id.c_str()), "set TTS voice", error);
    if (!ok) return false;
    const auto malformed = [&] {
        if (error) *error = "TTS returned malformed generation metadata";
        std::lock_guard queue_lock(impl.queue_mutex);
        if (!cancelled())
        {
            std::lock_guard statistics_lock(impl.statistics_mutex);
            impl.statistics.last_finish_reason = "invalid_metadata";
        }
        return false;
    };
    const auto parse_count = [](const char *value, std::uint64_t &parsed) {
        if (!value || !*value) return false;
        parsed = 0;
        for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(value); *cursor; ++cursor)
        {
            if (*cursor < '0' || *cursor > '9' ||
                parsed > (std::numeric_limits<std::uint64_t>::max() - (*cursor - '0')) / 10) return false;
            parsed = parsed * 10 + (*cursor - '0');
        }
        return true;
    };
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (cancelled())
        {
            if (error) *error = "TTS synthesis cancelled";
            return false;
        }
        {
            std::lock_guard queue_lock(impl.queue_mutex);
            if (cancelled())
            {
                if (error) *error = "TTS synthesis cancelled";
                return false;
            }
            std::lock_guard statistics_lock(impl.statistics_mutex);
            impl.statistics.last_finish_reason.clear();
        }
        const std::string token_limit = std::to_string(max_tokens);
        if (!impl.check(impl.request_set_option(request, "max_tokens", token_limit.c_str()), "set TTS generation budget", error))
            return false;
        {
            std::lock_guard queue_lock(impl.queue_mutex);
            if (cancelled())
            {
                if (error) *error = "TTS synthesis cancelled";
                return false;
            }
            std::lock_guard statistics_lock(impl.statistics_mutex);
            impl.statistics.max_tokens = max_tokens;
        }
        const auto attempt_key = std::to_string(attempt + 1);
        if (request_id != 0 && !impl.check(impl.request_set_option(request, "hmi.attempt", attempt_key.c_str()),
                "set TTS attempt trace", error)) return false;
        const char *strategy = attempt == 0 ? "initial" : "budget_retry";
        impl.trace(attempt != 0 ? base::LogLevel::Info : base::LogLevel::Debug,
            "attempt_started", request_id, segment_index,
            "attempt=" + attempt_key + " strategy=" + strategy + " budget=" + std::to_string(max_tokens) +
                voice_fields);
        {
            std::lock_guard queue_lock(impl.queue_mutex);
            if (cancelled())
            {
                if (error) *error = "TTS synthesis cancelled";
                return false;
            }
            if (attempt != 0)
            {
                std::lock_guard statistics_lock(impl.statistics_mutex);
                ++impl.statistics.generation_retries;
            }
        }
        const auto attempt_started = Impl::Clock::now();
        audiocpp_result *result = nullptr;
        ok = impl.check(impl.session_run(impl.session, request, &result), "synthesize Qwen3 speech", error);
        const std::unique_ptr<audiocpp_result, decltype(impl.result_free)> owned_result(result, impl.result_free);
        if (!ok) return false;
        if (cancelled())
        {
            if (error) *error = "TTS synthesis cancelled";
            return false;
        }
        if (!result)
        {
            if (error) *error = "TTS returned no synthesis result";
            return false;
        }
        const float *samples = nullptr;
        std::size_t frames = 0;
        int rate = 0, channels = 0;
        ok = impl.check(impl.result_audio(result, &samples, &frames, &rate, &channels), "read TTS PCM", error);
        if (!ok) return false;
        if (ok && (!samples || frames == 0 || rate < 8000 || rate > 192000 || channels < 1 || channels > 8 ||
                   frames > static_cast<std::size_t>(rate) * 300))
        {
            if (error) *error = "TTS returned empty, invalid or excessive PCM";
            return false;
        }
        bool has_generation = false;
        bool length_limited = false;
        std::string finish_reason = "unreported";
        if (impl.result_artifact_count)
        {
            const auto count = impl.result_artifact_count(result);
            if (count > 4096) return malformed();
            for (std::size_t index = 0; index < count; ++index)
            {
                audiocpp_artifact_kind kind = AUDIOCPP_ARTIFACT_CUSTOM;
                const char *id = nullptr;
                const void *payload = nullptr;
                std::size_t payload_bytes = 0;
                if (!impl.check(impl.result_artifact(result, index, &kind, &id, &payload, &payload_bytes),
                                "read TTS generation artifact", error)) return false;
                if (!id || std::strcmp(id, "qwen3_tts_generation") != 0) continue;
                if (has_generation || kind != AUDIOCPP_ARTIFACT_CUSTOM) return malformed();
                has_generation = true;
                bool has_reason = false, has_frames = false, has_budget = false, has_chunks = false;
                std::uint64_t generated_frames = 0, budget = 0, chunks = 1;
                const auto metadata_count = impl.result_artifact_meta_count(result, index);
                if (metadata_count > 4096) return malformed();
                for (std::size_t field = 0; field < metadata_count; ++field)
                {
                    const char *key = nullptr, *value = nullptr;
                    if (!impl.check(impl.result_artifact_meta(result, index, field, &key, &value),
                                    "read TTS generation metadata", error)) return false;
                    if (!key || !value) return malformed();
                    if (std::strcmp(key, "finish_reason") == 0)
                    {
                        if (has_reason || (std::strcmp(value, "eos") != 0 && std::strcmp(value, "length") != 0))
                            return malformed();
                        finish_reason = value;
                        has_reason = true;
                    }
                    else if (std::strcmp(key, "generated_frames") == 0)
                    {
                        if (has_frames || !parse_count(value, generated_frames)) return malformed();
                        has_frames = true;
                    }
                    else if (std::strcmp(key, "max_tokens") == 0)
                    {
                        if (has_budget || !parse_count(value, budget)) return malformed();
                        has_budget = true;
                    }
                    else if (std::strcmp(key, "generation_chunks") == 0)
                    {
                        if (has_chunks || !parse_count(value, chunks) || chunks == 0) return malformed();
                        has_chunks = true;
                    }
                }
                if (!has_reason || !has_frames || !has_budget || budget != static_cast<std::uint64_t>(max_tokens) ||
                    chunks > std::numeric_limits<std::uint64_t>::max() / budget ||
                    generated_frames > chunks * (budget - 1)) return malformed();
                length_limited = finish_reason == "length";
            }
        }
        if (!has_generation && rate == 24000 && channels == 1 &&
            frames == static_cast<std::size_t>(max_tokens - 1) * 1920)
        {
            length_limited = true;
            finish_reason = "length_heuristic";
        }
        {
            std::lock_guard queue_lock(impl.queue_mutex);
            if (cancelled())
            {
                if (error) *error = "TTS synthesis cancelled";
                return false;
            }
            std::lock_guard statistics_lock(impl.statistics_mutex);
            impl.statistics.last_finish_reason = finish_reason;
            if (length_limited) ++impl.statistics.limit_hits;
        }
        const auto attempt_ms = std::chrono::duration<double, std::milli>(Impl::Clock::now() - attempt_started).count();
        impl.trace(length_limited ? base::LogLevel::Warning :
                attempt != 0 ? base::LogLevel::Info : base::LogLevel::Debug,
            "attempt_finished", request_id, segment_index,
            "attempt=" + attempt_key + " strategy=" + strategy + " budget=" + std::to_string(max_tokens) +
            voice_fields +
            " finish_reason=" + finish_reason + " pcm_frames=" + std::to_string(frames) +
            " sample_rate=" + std::to_string(rate) + " channels=" + std::to_string(channels) +
            " audio_seconds=" + std::to_string(static_cast<double>(frames) / rate) +
            " wall_ms=" + std::to_string(attempt_ms));
        if (length_limited)
        {
            if (attempt == 0 && max_tokens < 2048)
            {
                max_tokens = std::min(max_tokens * 2, 2048);
                impl.trace(base::LogLevel::Info, "recovery_retry", request_id, segment_index,
                    "attempt=2 strategy=budget_retry budget=" + std::to_string(max_tokens) +
                    " text_hash=" + std::to_string(text_fingerprint(text)) + voice_fields);
                continue;
            }
            if (error) *error = "TTS generation reached its token limit; incomplete audio was discarded";
            return false;
        }
        out.sample_rate = rate;
        out.channels = channels;
        out.samples.assign(samples, samples + frames * static_cast<std::size_t>(channels));
        if (!std::all_of(out.samples.begin(), out.samples.end(), [](float value) { return std::isfinite(value); }))
        {
            if (error) *error = "TTS returned non-finite PCM";
            out = {};
            return false;
        }
        {
            std::lock_guard queue_lock(impl.queue_mutex);
            if (cancelled())
            {
                if (error) *error = "TTS synthesis cancelled";
                out = {};
                return false;
            }
            impl.remember_audio(text, context, out);
            if (!worker_request)
            {
                std::lock_guard statistics_lock(impl.statistics_mutex);
                impl.statistics.generated_audio_frames = frames;
                impl.statistics.audio_seconds = static_cast<double>(frames) / rate;
            }
        }
        return true;
    }
    return false;
}
}
