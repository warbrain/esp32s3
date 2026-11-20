#include "server/inference_session.hpp"

#include "app/voice_runtime.hpp"
#include "engine/native_client.hpp"
#include "engine/sdk_threads.hpp"
#include "engine/tts_session.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

namespace hmi::server
{
namespace
{
std::string utf8(const std::filesystem::path &path)
{
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}

std::string environment(const wchar_t *name, const std::string &fallback)
{
    wchar_t *value = nullptr;
    std::size_t size = 0;
    if (_wdupenv_s(&value, &size, name) != 0 || value == nullptr) return fallback;
    const std::unique_ptr<wchar_t, decltype(&std::free)> owned(value, &std::free);
    return *value ? utf8(std::filesystem::path(value)) : fallback;
}

bool fail(std::string *error, const std::string &message)
{
    if (error) *error = message;
    return false;
}
}

struct InferenceSession::Impl
{
    // Destruction order also keeps the registry alive until both sessions close.
    engine::Client client;
    engine::AsrSession asr;
    engine::TtsSession tts;
    engine::AsrConfig asr_config;
    engine::TtsConfig tts_config;
    std::mutex operation_mutex;
    mutable std::mutex health_mutex;
    InferenceHealth view;
    std::atomic<bool> opened{false};
    std::atomic<bool> native_ready{false};
    bool tts_opened = false;
    int threads = 4;
    std::size_t maximum_frames = 30 * 16000;

    void release()
    {
        opened.store(false);
        native_ready.store(false);
        tts.stop();
        tts_opened = false;
        asr.close();
        client.close();
    }

    void progress(const char *status)
    {
        std::lock_guard lock(health_mutex);
        view.status = status;
        view.busy = true;
        view.error.clear();
    }

    bool failed(const char *status, const std::string &message, std::string *error)
    {
        std::lock_guard lock(health_mutex);
        view.status = status;
        view.error = message;
        return fail(error, message);
    }

    bool ensure_native(std::string *error)
    {
        if (client.ready()) return true;
        progress("Loading native engine");
        std::string detail;
        if (!client.open(&detail))
        {
            native_ready.store(false);
            {
                std::lock_guard lock(health_mutex);
                view.ready = false;
            }
            return failed("Native engine unavailable", detail, error);
        }
        native_ready.store(true);
        std::lock_guard lock(health_mutex);
        view.ready = true;
        return true;
    }

    bool ensure_asr(std::string *error)
    {
        if (asr.ready()) return true;
        progress("Loading ASR model");
        std::string detail;
        if (!asr.open(client, asr_config, &detail))
            return failed("ASR unavailable", detail, error);
        std::lock_guard lock(health_mutex);
        view.asr_ready = true;
        view.backend = asr.statistics().backend;
        return true;
    }

    bool ensure_tts(std::string *error)
    {
        if (tts_opened) return true;
        progress("Opening TTS service");
        std::string detail;
        if (!tts.open(client, tts_config, &detail))
            return failed("TTS unavailable", detail, error);
        tts_opened = true;
        std::lock_guard lock(health_mutex);
        view.tts_ready = true;
        return true;
    }

    struct Activity
    {
        Impl &owner;
        explicit Activity(Impl &value, const char *status) : owner(value)
        {
            owner.progress(status);
        }
        ~Activity()
        {
            std::lock_guard lock(owner.health_mutex);
            owner.view.busy = false;
            if (owner.view.error.empty())
                owner.view.status = owner.native_ready.load() ? "Ready" : "Stopped";
        }
    };
};

InferenceSession::InferenceSession() : impl_(std::make_unique<Impl>()) {}
InferenceSession::~InferenceSession() { close(); }

bool InferenceSession::open(const app::VoiceOptions &options, std::string *error)
{
    std::lock_guard operation(impl_->operation_mutex);
    if (error) error->clear();
    impl_->release();
    {
        std::lock_guard lock(impl_->health_mutex);
        impl_->view = {};
    }
    if (options.threads < 1 || options.threads > 256 ||
        options.maximum_audio_seconds < 1 || options.maximum_audio_seconds > 30 ||
        !std::isfinite(options.preview_interval_seconds) || options.preview_interval_seconds <= 0.0 ||
        options.preview_interval_seconds > 30.0)
    {
        return impl_->failed("Invalid configuration",
            "Inference requires 1..256 threads, 1..30 seconds of audio, and a preview interval in (0, 30]", error);
    }

    try
    {
        impl_->threads = options.threads;
        impl_->maximum_frames = static_cast<std::size_t>(options.maximum_audio_seconds) * 16000;
        const engine::detail::ScopedSdkThreads sdk_threads(options.threads);
        const std::string backend = environment(L"HMI_VOICE_BACKEND", options.backend);
        {
            std::lock_guard lock(impl_->health_mutex);
            impl_->view.backend = backend;
            impl_->view.threads = options.threads;
        }
        engine::AsrConfig asr;
        asr.backend = backend;
        asr.threads = options.threads;
        asr.maximum_audio_seconds = options.maximum_audio_seconds;
        asr.preview_interval_seconds = options.preview_interval_seconds;
        asr.refine = options.refine;
        asr.model_path = environment(L"HMI_ASR_MODEL", options.asr_model.empty() ?
            asr.model_path : utf8(options.asr_model));
        engine::TtsConfig tts;
        tts.backend = backend;
        tts.threads = options.threads;
        tts.model_path = environment(L"HMI_TTS_MODEL", options.tts_model.empty() ?
            tts.model_path : utf8(options.tts_model));
        tts.reference_wav = environment(L"HMI_TTS_REFERENCE_WAV", "");
        impl_->asr_config = std::move(asr);
        impl_->tts_config = std::move(tts);
        impl_->opened.store(true);
        std::string detail;
        const bool native_ready = impl_->ensure_native(&detail);
        {
            std::lock_guard lock(impl_->health_mutex);
            impl_->view.busy = false;
            if (native_ready) impl_->view.status = "Ready";
        }
        return true;
    }
    catch (const std::exception &exception)
    {
        impl_->release();
        {
            std::lock_guard lock(impl_->health_mutex);
            impl_->view.ready = impl_->view.asr_ready = impl_->view.tts_ready = false;
            impl_->view.busy = false;
        }
        return impl_->failed("Invalid configuration", exception.what(), error);
    }
}

void InferenceSession::close()
{
    std::lock_guard operation(impl_->operation_mutex);
    impl_->release();
    std::lock_guard lock(impl_->health_mutex);
    impl_->view = {};
}

bool InferenceSession::ready() const noexcept
{
    return impl_->native_ready.load();
}

InferenceHealth InferenceSession::health() const
{
    InferenceHealth snapshot;
    {
        std::lock_guard lock(impl_->health_mutex);
        snapshot = impl_->view;
    }
    snapshot.tts_model_ready = impl_->tts.statistics().model_ready;
    return snapshot;
}

TranscriptionStatus InferenceSession::transcribe(std::span<const float> mono_16khz, engine::Transcript &out,
                                                std::string *error)
{
    out = {};
    if (error) error->clear();
    std::lock_guard operation(impl_->operation_mutex);
    if (!impl_->opened.load())
    {
        fail(error, "Inference service is not open");
        return TranscriptionStatus::Unavailable;
    }
    if (mono_16khz.empty() || mono_16khz.size() > impl_->maximum_frames)
    {
        fail(error, "ASR audio must be nonempty and at most " + std::to_string(impl_->maximum_frames / 16000) + " s");
        return TranscriptionStatus::InvalidAudio;
    }
    if (!std::all_of(mono_16khz.begin(), mono_16khz.end(), [](float sample) {
            return std::isfinite(sample) && sample >= -1.0f && sample <= 1.0f;
        }))
    {
        fail(error, "ASR audio must contain finite normalized samples");
        return TranscriptionStatus::InvalidAudio;
    }

    const Impl::Activity activity(*impl_, "Recognizing");
    const engine::detail::ScopedSdkThreads sdk_threads(impl_->threads);
    try
    {
        std::string detail;
        if (!impl_->ensure_native(error)) return TranscriptionStatus::Unavailable;
        if (!impl_->ensure_asr(error)) return TranscriptionStatus::Failed;
        impl_->progress("Recognizing");
        if (!impl_->asr.begin(&detail))
        {
            impl_->failed("ASR failed", detail, error);
            return TranscriptionStatus::Failed;
        }
        std::vector<engine::Transcript> previews;
        if (!impl_->asr.push(mono_16khz.data(), mono_16khz.size(), previews, &detail, false) ||
            !impl_->asr.finish(out, &detail))
        {
            impl_->asr.reset();
            out = {};
            impl_->failed("ASR failed", detail, error);
            return TranscriptionStatus::Failed;
        }
        return TranscriptionStatus::Success;
    }
    catch (const std::exception &exception)
    {
        impl_->asr.reset();
        out = {};
        impl_->failed("ASR failed", exception.what(), error);
        return TranscriptionStatus::Failed;
    }
}

bool InferenceSession::synthesize(std::string_view text, audio::AudioBuffer &out, std::string *error)
{
    out = {};
    if (error) error->clear();
    std::lock_guard operation(impl_->operation_mutex);
    if (!impl_->opened.load()) return fail(error, "Inference service is not open");
    const Impl::Activity activity(*impl_, "Synthesizing");
    const engine::detail::ScopedSdkThreads sdk_threads(impl_->threads);
    try
    {
        std::string detail;
        if (!impl_->ensure_native(error) || !impl_->ensure_tts(error)) return false;
        impl_->progress("Synthesizing");
        if (!impl_->tts.synthesize(text, out, &detail))
        {
            out = {};
            return impl_->failed("TTS failed", detail, error);
        }
        return true;
    }
    catch (const std::exception &exception)
    {
        out = {};
        return impl_->failed("TTS failed", exception.what(), error);
    }
}
}
