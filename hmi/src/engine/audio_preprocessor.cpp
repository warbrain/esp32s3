#include "engine/audio_preprocessor.hpp"
#include "engine/native_client.hpp"
#include "engine/sdk_library.hpp"
#include "engine/sdk_threads.hpp"
#include "base/project_paths.hpp"

#include <audiocpp.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace hmi::engine
{
struct AudioPreprocessor::Impl
{
    HMODULE module = nullptr;
    audiocpp_audio_frontend *frontend = nullptr;
    bool opened = false;
    bool active = false;
    std::size_t maximum_frames = 480000;
    AudioPreprocessorStatistics stats;
    decltype(&::audiocpp_last_error) last_error = nullptr;
    decltype(&::audiocpp_audio_frontend_create) create = nullptr;
    decltype(&::audiocpp_audio_frontend_reset) reset = nullptr;
    decltype(&::audiocpp_audio_frontend_push) push = nullptr;
    decltype(&::audiocpp_audio_frontend_finish) finish = nullptr;
    decltype(&::audiocpp_audio_frontend_get_statistics) get_statistics = nullptr;
    decltype(&::audiocpp_audio_frontend_free) free = nullptr;
    decltype(&::audiocpp_result_audio) result_audio = nullptr;
    decltype(&::audiocpp_result_free) result_free = nullptr;

    bool fail(const std::string &detail, std::string *error)
    {
        stats.failed = true;
        if (error) *error = detail;
        return false;
    }

    bool check(audiocpp_status status, const char *operation, std::string *error)
    {
        if (status == AUDIOCPP_OK) return true;
        const char *detail = last_error ? last_error() : nullptr;
        return fail(std::string(operation) + ": " + (detail && *detail ?
            detail::native_diagnostic(detail) : "native audio processing failed"), error);
    }

    bool refresh(std::string *error)
    {
        if (!frontend) return true;
        audiocpp_audio_frontend_statistics value{};
        value.struct_size = sizeof(value);
        if (!check(get_statistics(frontend, &value), "read audio processing statistics", error)) return false;
        if (value.version != AUDIOCPP_AUDIO_FRONTEND_STATISTICS_VERSION ||
            value.output_frames > value.input_frames || value.input_frames > maximum_frames ||
            !std::isfinite(value.latest_vad_probability) || !std::isfinite(value.max_vad_probability))
            return fail("invalid native audio processing statistics", error);
        stats.ready = opened;
        stats.vad_enabled = (value.flags & AUDIOCPP_AUDIO_FRONTEND_VAD_ENABLED) != 0;
        stats.denoise_enabled = (value.flags & AUDIOCPP_AUDIO_FRONTEND_GTCRN_ENABLED) != 0;
        stats.failed = stats.failed || (value.flags & AUDIOCPP_AUDIO_FRONTEND_FAILED) != 0;
        stats.finished = (value.flags & AUDIOCPP_AUDIO_FRONTEND_FINISHED) != 0;
        stats.request_id = value.request_id;
        stats.input_frames = value.input_frames;
        stats.output_frames = value.output_frames;
        stats.gtcrn_frames = value.gtcrn_frames;
        stats.vad_chunks = value.vad_chunks;
        stats.voiced_chunks = value.voiced_chunks;
        stats.uncertain_chunks = value.uncertain_chunks;
        stats.voiced_frames = value.voiced_frames;
        stats.pending_frames = value.pending_frames;
        stats.latest_vad_probability = value.latest_vad_probability;
        stats.max_vad_probability = value.max_vad_probability;
        stats.speech_seen = value.speech_seen != 0;
        stats.speech_active = value.speech_active != 0;
        stats.gtcrn_ms = value.gtcrn_ms;
        stats.vad_ms = value.vad_ms;
        stats.wall_ms = value.wall_ms;
        return true;
    }

    bool copy_result(audiocpp_result *raw, std::vector<float> &out, std::string *error)
    {
        const std::unique_ptr<audiocpp_result, decltype(result_free)> owned(raw, result_free);
        if (!owned) return true;
        const float *samples = nullptr;
        std::size_t frames = 0;
        int sample_rate = 0, channels = 0;
        if (!check(result_audio(owned.get(), &samples, &frames, &sample_rate, &channels),
                "read processed audio", error)) return false;
        if ((frames != 0 && !samples) || sample_rate != 16000 || channels != 1 || frames > maximum_frames ||
            (frames != 0 && !std::all_of(samples, samples + frames, [](float sample) { return std::isfinite(sample); })))
            return fail("invalid native processed audio", error);
        if (frames) out.assign(samples, samples + frames);
        return true;
    }
};

AudioPreprocessor::AudioPreprocessor() : impl_(std::make_unique<Impl>()) {}
AudioPreprocessor::~AudioPreprocessor() { close(); }

bool AudioPreprocessor::open(Client &client, const AudioPreprocessorConfig &config, std::string *error)
{
    close();
    if (error) error->clear();
    auto &impl = *impl_;
    if (!std::isfinite(config.vad_threshold) || config.vad_threshold <= 0.0f || config.vad_threshold >= 1.0f ||
        config.maximum_audio_seconds < 1 || config.maximum_audio_seconds > 30)
        return impl.fail("invalid audio processing threshold or duration limit", error);
    impl.maximum_frames = static_cast<std::size_t>(config.maximum_audio_seconds) * 16000;
    if (!config.vad_enabled && !config.denoise_enabled)
    {
        impl.opened = true;
        impl.stats.ready = true;
        return true;
    }
    if (!client.ready()) return impl.fail("audio processing requires an open native engine", error);
    std::filesystem::path vad, denoise;
    if (config.vad_enabled && !base::resolve_project_path(config.vad_model, base::process_directory(), vad, error)) return false;
    if (config.denoise_enabled && !base::resolve_project_path(config.denoise_model, base::process_directory(), denoise, error)) return false;
    const auto utf8 = [](const std::filesystem::path &path) {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    };
    const auto vad_path = utf8(vad), denoise_path = utf8(denoise);
    impl.module = detail::retain_sdk_library(client.module_handle());
    if (!impl.module) return impl.fail("cannot retain native audio processing engine", error);
    const auto bind = [&impl, error](const char *name, auto &target) {
        target = reinterpret_cast<std::decay_t<decltype(target)>>(GetProcAddress(impl.module, name));
        return target != nullptr || impl.fail("native audio processing interface missing: " +
            detail::native_diagnostic(name), error);
    };
    if (!bind("audiocpp_last_error", impl.last_error) ||
        !bind("audiocpp_audio_frontend_create", impl.create) ||
        !bind("audiocpp_audio_frontend_reset", impl.reset) ||
        !bind("audiocpp_audio_frontend_push", impl.push) ||
        !bind("audiocpp_audio_frontend_finish", impl.finish) ||
        !bind("audiocpp_audio_frontend_get_statistics", impl.get_statistics) ||
        !bind("audiocpp_audio_frontend_free", impl.free) ||
        !bind("audiocpp_result_audio", impl.result_audio) || !bind("audiocpp_result_free", impl.result_free))
    { close(); return false; }
    audiocpp_audio_frontend_config selected{};
    selected.struct_size = sizeof(selected);
    selected.cpu_threads = 1;
    selected.vad_model_path = vad_path.empty() ? nullptr : vad_path.c_str();
    selected.gtcrn_model_path = denoise_path.empty() ? nullptr : denoise_path.c_str();
    selected.vad_threshold = config.vad_threshold;
    selected.max_frames = static_cast<std::uint32_t>(impl.maximum_frames);
    const detail::ScopedSdkThreads threads(1);
    if (!impl.check(impl.create(&selected, &impl.frontend), "open audio processing", error)) { close(); return false; }
    if (!impl.frontend) { close(); return impl.fail("native audio processing returned no handle", error); }
    impl.opened = true;
    if (!impl.refresh(error)) { close(); return false; }
    return true;
}

void AudioPreprocessor::close()
{
    auto &impl = *impl_;
    if (impl.frontend && impl.free) impl.free(impl.frontend);
    impl.frontend = nullptr;
    if (impl.module) FreeLibrary(impl.module);
    impl.module = nullptr;
    impl.opened = false;
    impl.active = false;
    impl.stats = {};
}

bool AudioPreprocessor::ready() const noexcept { return impl_->opened; }

bool AudioPreprocessor::begin(std::uint64_t request_id, std::string *error)
{
    if (error) error->clear();
    auto &impl = *impl_;
    if (!impl.opened) return impl.fail("audio processing is not ready", error);
    const bool vad = impl.stats.vad_enabled, denoise = impl.stats.denoise_enabled;
    impl.stats = {};
    impl.stats.ready = true;
    impl.stats.vad_enabled = vad;
    impl.stats.denoise_enabled = denoise;
    impl.stats.request_id = request_id;
    impl.active = false;
    const detail::ScopedSdkThreads threads(1);
    if (impl.frontend && (!impl.check(impl.reset(impl.frontend, request_id), "reset audio processing", error) ||
            !impl.refresh(error))) return false;
    impl.active = true;
    return true;
}

bool AudioPreprocessor::push(const float *samples, std::size_t frames, std::vector<float> &out, std::string *error)
{
    out.clear();
    if (error) error->clear();
    auto &impl = *impl_;
    if (!impl.active || impl.stats.failed || impl.stats.finished || (!samples && frames) ||
        frames > impl.maximum_frames - std::min<std::uint64_t>(impl.stats.input_frames, impl.maximum_frames) ||
        (frames && !std::all_of(samples, samples + frames, [](float sample) { return std::isfinite(sample); })))
        return impl.fail("invalid audio processing input or request state", error);
    if (!impl.frontend)
    {
        if (frames) out.assign(samples, samples + frames);
        impl.stats.input_frames += frames;
        impl.stats.output_frames += frames;
        return true;
    }
    const auto previous_output = impl.stats.output_frames;
    const detail::ScopedSdkThreads threads(1);
    audiocpp_result *raw = nullptr;
    const auto status = impl.push(impl.frontend, samples, frames, &raw);
    const bool copied = impl.copy_result(raw, out, error);
    if (!impl.check(status, "process microphone audio", error) || !copied || !impl.refresh(error))
    { out.clear(); return false; }
    if (impl.stats.output_frames - previous_output != out.size())
    { out.clear(); return impl.fail("native audio processing output count mismatch", error); }
    return true;
}

bool AudioPreprocessor::finish(std::vector<float> &out, std::string *error)
{
    out.clear();
    if (error) error->clear();
    auto &impl = *impl_;
    if (!impl.active || impl.stats.failed || impl.stats.finished)
        return impl.fail("audio processing request is not active", error);
    if (!impl.frontend) { impl.stats.finished = true; impl.active = false; return true; }
    const auto previous_output = impl.stats.output_frames;
    const detail::ScopedSdkThreads threads(1);
    audiocpp_result *raw = nullptr;
    const auto status = impl.finish(impl.frontend, &raw);
    const bool copied = impl.copy_result(raw, out, error);
    if (!impl.check(status, "finish microphone audio processing", error) || !copied || !impl.refresh(error))
    { out.clear(); return false; }
    if (impl.stats.output_frames - previous_output != out.size() || impl.stats.output_frames != impl.stats.input_frames ||
        impl.stats.pending_frames != 0 || !impl.stats.finished)
    { out.clear(); return impl.fail("native audio processing lost final samples", error); }
    impl.active = false;
    return true;
}

AudioPreprocessorStatistics AudioPreprocessor::statistics() const noexcept { return impl_->stats; }
}
