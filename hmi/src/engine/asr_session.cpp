#include "engine/asr_session.hpp"
#include "engine/sdk_library.hpp"
#include "engine/sdk_session.hpp"
#include "engine/sdk_threads.hpp"
#include "base/project_paths.hpp"

#include <audiocpp.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <chrono>
#include <utility>

namespace hmi::engine
{
namespace
{
using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct PreviewSchedule
{
    std::size_t interval_frames = 16000;
    std::size_t next_frames = 16000;

    void configure(double seconds)
    {
        interval_frames = std::max<std::size_t>(1,
            static_cast<std::size_t>(std::ceil(seconds * 16000.0)));
        next_frames = interval_frames;
    }

    bool due(std::size_t frames) const noexcept { return frames >= next_frames; }

    void complete(std::size_t frames, double milliseconds)
    {
        // Keep live drafts at the configured cadence. Only actual backend
        // cost can delay them; a faster subsequent decode restores the cadence.
        // Slow backends get twice their last compute duration of new audio.
        const double adaptive_frames = std::ceil(std::max(0.0, milliseconds) * 32.0);
        const auto delay = static_cast<std::size_t>(std::min(adaptive_frames, 16000.0 * 60.0));
        next_frames = frames + std::max(interval_frames, delay);
    }
};

struct PrefixApi
{
    decltype(&::audiocpp_last_error) last_error = nullptr;
    decltype(&::audiocpp_request_create) request_create = nullptr;
    decltype(&::audiocpp_request_free) request_free = nullptr;
    decltype(&::audiocpp_request_set_audio) request_set_audio = nullptr;
    decltype(&::audiocpp_request_set_option) request_set_option = nullptr;
    decltype(&::audiocpp_stream_start) stream_start = nullptr;
    decltype(&::audiocpp_stream_push) stream_push = nullptr;
    decltype(&::audiocpp_stream_next_event) stream_next_event = nullptr;
    decltype(&::audiocpp_stream_finish) stream_finish = nullptr;
    decltype(&::audiocpp_stream_reset) stream_reset = nullptr;
    decltype(&::audiocpp_event_free) event_free = nullptr;
    decltype(&::audiocpp_result_free) result_free = nullptr;
    decltype(&::audiocpp_result_text) result_text = nullptr;

    bool check(audiocpp_status status, const char *operation, std::string *error) const
    {
        if (status == AUDIOCPP_OK) return true;
        if (error)
        {
            const char *detail = last_error ? last_error() : nullptr;
            *error = std::string(operation) + ": " +
                (detail && *detail ? hmi::engine::detail::native_diagnostic(detail) : "engine call failed");
        }
        return false;
    }
};

bool start_prefix_stream(const PrefixApi &api, audiocpp_session *session,
                         const AsrConfig &config, std::string *error,
                         std::uint64_t request_id, std::size_t revision, const char *stage)
{
    const std::unique_ptr<audiocpp_request, decltype(api.request_free)> request(
        api.request_create(), api.request_free);
    if (!request)
    {
        if (error) *error = "cannot allocate ASR request";
        return false;
    }
    if (!api.check(api.request_set_audio(request.get(), nullptr, 0, 16000, 1),
                   "set ASR audio format", error)) return false;
    if (!config.language.empty() &&
        !api.check(api.request_set_option(request.get(), "language", config.language.c_str()),
                   "set ASR language", error)) return false;
    // One SDK window covers the complete prefix. Its decoder can revise words
    // recognized earlier; native short windows only concatenate independent text.
    const auto window = std::to_string(config.maximum_audio_seconds);
    if (!api.check(api.request_set_option(request.get(), "audio_chunk_duration_sec", window.c_str()),
                   "set ASR prefix window", error)) return false;
    const auto id = std::to_string(request_id);
    const auto update = std::to_string(revision);
    if (!api.check(api.request_set_option(request.get(), "hmi.request_id", id.c_str()), "set ASR request identity", error) ||
        !api.check(api.request_set_option(request.get(), "hmi.revision", update.c_str()), "set ASR revision", error) ||
        !api.check(api.request_set_option(request.get(), "hmi.stage", stage), "set ASR stage", error)) return false;
    return api.check(api.stream_start(session, request.get()), "start ASR prefix stream", error);
}

bool transcribe_prefix(const PrefixApi &api, audiocpp_session *session,
                       const AsrConfig &config, const std::vector<float> &audio,
                       std::size_t chunk, Transcript &out, std::string *error,
                       std::uint64_t request_id, std::size_t revision, const char *stage)
{
    const detail::ScopedSdkThreads threads(config.threads);
    out = {};
    if (!session || chunk == 0 || audio.empty() ||
        audio.size() > static_cast<std::size_t>(config.maximum_audio_seconds) * 16000)
    {
        if (error) *error = "invalid ASR prefix audio or transport chunk";
        return false;
    }
    if (!start_prefix_stream(api, session, config, error, request_id, revision, stage)) return false;
    struct ResetStream
    {
        const PrefixApi &api;
        audiocpp_session *session;
        ~ResetStream() { (void)api.stream_reset(session); }
    } reset{api, session};
    std::int64_t offset = 0;
    while (static_cast<std::size_t>(offset) < audio.size())
    {
        const auto frames = std::min(chunk, audio.size() - static_cast<std::size_t>(offset));
        audiocpp_event *event = nullptr;
        const auto status = api.stream_push(session, audio.data() + offset, frames,
                                            16000, 1, offset, &event);
        const bool pushed = api.check(status, "ASR prefix push", error);
        api.event_free(event);
        if (!pushed) return false;
        offset += static_cast<std::int64_t>(frames);
        for (;;)
        {
            event = nullptr;
            const auto queued = api.stream_next_event(session, &event);
            const bool drained = api.check(queued, "ASR drain prefix event", error);
            const bool empty = event == nullptr;
            api.event_free(event);
            if (!drained) return false;
            if (empty) break;
        }
    }
    audiocpp_result *raw = nullptr;
    const auto status = api.stream_finish(session, &raw);
    const std::unique_ptr<audiocpp_result, decltype(api.result_free)> result(raw, api.result_free);
    if (!api.check(status, "finish ASR prefix", error)) return false;
    if (!result)
    {
        if (error) *error = "ASR stream finish returned no result";
        return false;
    }
    const char *text = nullptr;
    const auto text_status = api.result_text(result.get(), &text, nullptr);
    if (text_status == AUDIOCPP_OK && text) out.text = text;
    else if (text_status != AUDIOCPP_ERR_NOT_AVAILABLE &&
             !api.check(text_status, "read ASR prefix text", error)) return false;
    out.final = true;
    out.confidence = -1.0f;
    return true;
}
}

struct AsrSession::Impl
{
    HMODULE module = nullptr;
    audiocpp_model *model = nullptr;
    audiocpp_session *session = nullptr;
    AsrConfig config;
    AsrStatistics statistics;
    PreviewSchedule schedule;
    std::size_t chunk = 16000;
    bool active = false;
    std::vector<float> audio;
    Transcript last_preview;
    std::size_t last_preview_frames = 0;
    std::uint64_t request_id = 0;
    PrefixApi api;

    decltype(&::audiocpp_model_load) model_load = nullptr;
    decltype(&::audiocpp_model_free) model_free = nullptr;
    decltype(&::audiocpp_session_create) session_create = nullptr;
    decltype(&::audiocpp_session_free) session_free = nullptr;
    decltype(&::audiocpp_stream_policy) stream_policy = nullptr;

    bool bind(std::string *error)
    {
        const auto load = [this, error](const char *name, auto &target) {
            target = reinterpret_cast<std::decay_t<decltype(target)>>(GetProcAddress(module, name));
            if (!target && error) *error = "native ASR interface missing: " + detail::native_diagnostic(name);
            return target != nullptr;
        };
#define HMI_ASR_BIND(name) if (!load("audiocpp_" #name, name)) return false
        HMI_ASR_BIND(model_load); HMI_ASR_BIND(model_free);
        HMI_ASR_BIND(session_create); HMI_ASR_BIND(session_free);
        HMI_ASR_BIND(stream_policy);
#undef HMI_ASR_BIND
#define HMI_ASR_BIND(name) if (!load("audiocpp_" #name, api.name)) return false
        HMI_ASR_BIND(last_error);
        HMI_ASR_BIND(request_create); HMI_ASR_BIND(request_free);
        HMI_ASR_BIND(request_set_audio); HMI_ASR_BIND(request_set_option);
        HMI_ASR_BIND(stream_start); HMI_ASR_BIND(stream_push); HMI_ASR_BIND(stream_next_event);
        HMI_ASR_BIND(stream_finish); HMI_ASR_BIND(stream_reset); HMI_ASR_BIND(event_free);
        HMI_ASR_BIND(result_free); HMI_ASR_BIND(result_text);
#undef HMI_ASR_BIND
        return true;
    }

    void clear_utterance()
    {
        active = false;
        audio.clear();
        last_preview = {};
        last_preview_frames = 0;
        statistics.audio_frames = 0;
        statistics.preview_count = 0;
        statistics.preview_ms = 0.0;
        statistics.refine_ms = 0.0;
        schedule.configure(config.preview_interval_seconds);
    }
};

AsrSession::AsrSession() : impl_(std::make_unique<Impl>()) {}
AsrSession::~AsrSession() { close(); }

bool AsrSession::open(Client &client, const AsrConfig &config, std::string *error)
{
    close();
    if (error) error->clear();
    if (!client.ready() || config.model_path.empty() || config.threads < 1 || config.threads > 256 ||
        !std::isfinite(config.preview_interval_seconds) || config.preview_interval_seconds <= 0.0 ||
        config.preview_interval_seconds > 30.0 || config.maximum_audio_seconds < 1 ||
        config.maximum_audio_seconds > 30)
    {
        if (error) *error = "ASR requires an open engine, valid model, positive preview interval, and 1..30 second audio limit";
        return false;
    }
    std::filesystem::path model_path;
    if (!base::resolve_project_path(std::filesystem::path(std::u8string(config.model_path.begin(),
            config.model_path.end())), base::process_directory(), model_path, error)) return false;
    AsrConfig selected = config;
    const auto model_utf8 = model_path.generic_u8string();
    selected.model_path.assign(model_utf8.begin(), model_utf8.end());
    Impl &impl = *impl_;
    impl.module = detail::retain_sdk_library(client.module_handle());
    if (!impl.module)
    {
        if (error) *error = "cannot retain the loaded native engine";
        return false;
    }
    if (!impl.bind(error)) { close(); return false; }
    impl.config = std::move(selected);
    const audiocpp_model_config selection{"qwen3_asr", nullptr, nullptr, nullptr};
    if (!impl.api.check(impl.model_load(client.registry(), impl.config.model_path.c_str(), &selection, nullptr, &impl.model),
                        "load Qwen3-ASR model", error) ||
        !detail::create_sdk_session({impl.session_create, impl.session_free, impl.api.last_error},
            impl.model, "asr", "streaming", config.backend, config.threads, &impl.session, error,
            &impl.statistics.backend))
    {
        close(); return false;
    }
    impl.statistics.threads = config.threads;
    audiocpp_stream_input_kind input = AUDIOCPP_STREAM_INPUT_NONE;
    audiocpp_stream_output_kind output = AUDIOCPP_STREAM_OUTPUT_FINAL_RESULT;
    std::int64_t samples = 0;
    double seconds = 0.0;
    if (!impl.api.check(impl.stream_policy(impl.session, &input, &output, &samples, &seconds),
                        "read ASR policy", error))
    {
        close(); return false;
    }
    if (input != AUDIOCPP_STREAM_INPUT_AUDIO_CHUNKS)
    {
        if (error) *error = "ASR model does not accept streaming PCM";
        close(); return false;
    }
    // Keep feeds at or below one second. Prefix snapshots are SDK redecodes,
    // not token-level native streaming or cross-prefix KV-cache reuse.
    if (samples > 0 && samples <= 16000)
        impl.chunk = static_cast<std::size_t>(samples);
    else if (std::isfinite(seconds) && seconds > 0.0 && seconds <= 1.0)
        impl.chunk = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(seconds * 16000.0)));
    else
        impl.chunk = 16000;
    impl.audio.reserve(static_cast<std::size_t>(config.maximum_audio_seconds) * 16000);
    impl.clear_utterance();
    return true;
}

void AsrSession::close()
{
    Impl &impl = *impl_;
    if (impl.session && impl.session_free) impl.session_free(impl.session);
    if (impl.model && impl.model_free) impl.model_free(impl.model);
    impl.session = nullptr;
    impl.model = nullptr;
    if (impl.module) FreeLibrary(impl.module);
    impl.module = nullptr;
    impl.api = {};
    impl.model_load = nullptr;
    impl.model_free = nullptr;
    impl.session_create = nullptr;
    impl.session_free = nullptr;
    impl.stream_policy = nullptr;
    impl.clear_utterance();
    impl.statistics = {};
    impl.chunk = 16000;
}

bool AsrSession::ready() const noexcept { return impl_->session != nullptr; }
std::size_t AsrSession::preferred_chunk_samples() const noexcept
{
    return std::min<std::size_t>(impl_->chunk, 4000);
}
AsrStatistics AsrSession::statistics() const { return impl_->statistics; }

bool AsrSession::begin(std::string *error, std::uint64_t request_id)
{
    if (error) error->clear();
    Impl &impl = *impl_;
    if (!ready())
    {
        if (error) *error = "ASR session is not ready";
        return false;
    }
    reset();
    impl.request_id = request_id;
    impl.active = true;
    return true;
}

bool AsrSession::push(const float *samples, std::size_t frames, std::vector<Transcript> &out,
                      std::string *error, bool allow_preview)
{
    Impl &impl = *impl_;
    if (error) error->clear();
    const auto maximum_frames = static_cast<std::size_t>(impl.config.maximum_audio_seconds) * 16000;
    if (!impl.active || (!samples && frames != 0) || impl.audio.size() > maximum_frames ||
        frames > maximum_frames - impl.audio.size())
    {
        if (error) *error = "invalid ASR stream state or utterance exceeds its audio limit";
        return false;
    }
    if (frames == 0) return true;
    if (!std::all_of(samples, samples + frames, [](float value) { return std::isfinite(value); }))
    {
        if (error) *error = "ASR PCM contains non-finite samples";
        return false;
    }
    impl.audio.insert(impl.audio.end(), samples, samples + frames);
    impl.statistics.audio_frames = impl.audio.size();
    // Once capture has ended, only drain PCM here. finish() will decode the
    // complete audio, so a temporary preview would duplicate pending final work.
    if (!allow_preview || !impl.schedule.due(impl.audio.size())) return true;
    Transcript preview;
    const auto started = Clock::now();
    const bool ok = transcribe_prefix(impl.api, impl.session, impl.config, impl.audio,
                                      impl.chunk, preview, error, impl.request_id, impl.statistics.preview_count + 1, "preview");
    const double cost = elapsed_ms(started);
    ++impl.statistics.preview_count;
    impl.statistics.preview_ms += cost;
    impl.schedule.complete(impl.audio.size(), cost);
    if (!ok) { impl.active = false; return false; }
    preview.final = false;
    impl.last_preview = preview;
    impl.last_preview_frames = impl.audio.size();
    out.push_back(std::move(preview));
    return true;
}

bool AsrSession::finish(Transcript &out, std::string *error)
{
    out = {};
    if (error) error->clear();
    Impl &impl = *impl_;
    if (!impl.active || impl.audio.empty())
    {
        if (error) *error = "ASR stream is not active or contains no audio";
        return false;
    }
    impl.active = false;
    if (!impl.config.refine && impl.last_preview_frames == impl.audio.size())
    {
        out = impl.last_preview;
        out.final = true;
        impl.audio.clear();
        return true;
    }
    const auto started = Clock::now();
    const bool ok = transcribe_prefix(impl.api, impl.session, impl.config, impl.audio,
                                      impl.chunk, out, error, impl.request_id, impl.statistics.preview_count + 1, "final");
    impl.statistics.refine_ms = elapsed_ms(started);
    impl.audio.clear();
    out.final = true;
    out.confidence = -1.0f;
    return ok;
}

void AsrSession::reset()
{
    Impl &impl = *impl_;
    if (impl.session && impl.api.stream_reset) (void)impl.api.stream_reset(impl.session);
    impl.clear_utterance();
}
}
