#pragma once

#include "audio/audio_render.hpp"
#include "engine/asr_session.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace hmi::app { struct VoiceOptions; }

namespace hmi::server
{
struct InferenceHealth
{
    bool ready = false; // The native engine is available. Models load on demand.
    bool asr_ready = false;
    bool tts_ready = false;
    bool tts_model_ready = false;
    bool busy = false;
    int threads = 0;
    std::string backend;
    std::string status = "Stopped";
    std::string error;
};

enum class TranscriptionStatus
{
    Success,
    InvalidAudio,
    Unavailable,
    Failed,
};

// Owns the shared SDK registry and reusable recognition/synthesis sessions.
// Inference calls are synchronous and serialized. Health queries do not wait
// for inference. This service never opens capture devices or plays audio.
class InferenceSession
{
public:
    InferenceSession();
    ~InferenceSession();
    InferenceSession(const InferenceSession &) = delete;
    InferenceSession &operator=(const InferenceSession &) = delete;

    // A valid configuration opens the service even if the SDK is currently
    // unavailable. health() reports that state and requests retry initialization.
    bool open(const app::VoiceOptions &options, std::string *error);
    void close();
    bool ready() const noexcept;
    InferenceHealth health() const;

    // The input is complete mono PCM at 16 kHz with samples in [-1, 1].
    TranscriptionStatus transcribe(std::span<const float> mono_16khz, engine::Transcript &out, std::string *error);
    // The returned owned PCM retains the model's output rate and channel count.
    bool synthesize(std::string_view text, audio::AudioBuffer &out, std::string *error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
