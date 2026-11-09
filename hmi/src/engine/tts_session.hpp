#pragma once

#include "audio/audio_render.hpp"
#include "audio/stream_playback.hpp"
#include "engine/native_client.hpp"
#include "engine/model_location.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace hmi::base { class Logger; }

namespace hmi::engine
{
struct TtsConfig
{
    std::string model_path = default_model_path("tts.gguf");
    std::string backend = "auto";
    std::string language = "zh";
    std::string voice_id = "serena";
    std::string reference_wav; // Optional PCM16/float WAV for Base checkpoints.
    std::string instruction; // Optional fixed style prompt for CustomVoice/VoiceDesign.
    int threads = 4;
    base::Logger *logger = nullptr; // Borrowed until stop() returns.
    std::string run_id; // Correlates detached playback acknowledgements.
    // Optional playback backend. Empty uses the local WASAPI renderer; the
    // ESP32 bridge installs one that pushes Opus to the board instead.
    std::function<bool(const audio::detail::ReadAudio &, std::string *,
                       const std::function<bool()> &)> playback;
};

struct TtsStatistics
{
    bool model_ready = false;
    std::string backend;
    std::size_t segments = 0;
    std::size_t generated_segments = 0;
    std::size_t generated_audio_frames = 0;
    double audio_seconds = 0.0; // Generated PCM duration, not playback progress.
    std::size_t generation_retries = 0;
    std::size_t limit_hits = 0;
    std::string last_finish_reason;
    std::string pipeline_finish_reason;
    std::size_t queue_high_water = 0;
    double first_audio_ms = 0.0; // speak() acceptance to PCM ready, not the first audible sample.
    double queue_ms = 0.0; // speak() acceptance to the worker starting this utterance.
    double synthesis_ms = 0.0;
    double total_ms = 0.0;
    double model_load_ms = 0.0;
    std::size_t last_request_cache_hits = 0; // Most recent segment/SDK request only.
    std::size_t utterance_cache_hits = 0; // Accepted speak() request, including every segment.
    std::size_t recovery_splits = 0;
    std::size_t recovery_attempts = 0;
    bool recovery_exhausted = false;
    std::size_t cache_entries = 0;
    std::size_t cache_bytes = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t idle_unloads = 0;
    int idle_timeout_seconds = 60;
    std::string perf_mode;
    bool sampling_enabled = false;
    std::size_t sampling_fallbacks = 0; // Legacy counter; recovery keeps the sampler policy unchanged.
    int max_tokens = 0; // Budget of the most recent SDK request, excluding PCM cache hits.
    std::uint64_t request_id = 0;
    double producer_wait_ms = 0.0;
    double consumer_wait_ms = 0.0;
    std::size_t consumer_pending_reads = 0;
    std::size_t consumed_chunks = 0;
    audio::PlaybackStatistics playback;
};

// Client must remain open until stop(). Loading and synthesis happen in the
// worker. Only the newest pending utterance is retained.
class TtsSession
{
public:
    TtsSession();
    ~TtsSession();
    TtsSession(const TtsSession &) = delete;
    TtsSession &operator=(const TtsSession &) = delete;

    bool open(Client &client, const TtsConfig &config, std::string *error);
    // Queue model/session loading only; never synthesizes, caches or plays PCM.
    // Repeated requests coalesce and speech takes priority. Does not affect
    // busy() or the last speech statistics; synchronous synthesis rejects it.
    bool prepare(std::uint64_t request_id = 0);
    bool speak(std::string_view text);
    bool speak(std::string_view text, std::uint64_t request_id);
    // Cancel pending speech and playback without waiting for an SDK call.
    // The service remains open; a later speak() may queue a new utterance.
    void cancel(std::string_view reason = "cancelled");
    // Defer idle unloading of an already loaded model for 1..45 seconds.
    // Zero clears the lease. Invalid values, an unloaded model, or a release
    // already in progress return false. Never loads or waits for SDK work.
    bool defer_idle_unload(int seconds);
    void stop();
    bool busy() const noexcept;
    std::string last_error() const;
    // Detached snapshot; does not wait for an in-flight SDK synthesis call.
    TtsStatistics statistics() const;

    // Synchronous, owned PCM for validation. Never call on the UI thread.
    // Rejects queued/active worker speech or another synchronous call without waiting.
    bool synthesize(std::string_view text, audio::AudioBuffer &out, std::string *error);

private:
    bool synthesize_internal(std::string_view text, audio::AudioBuffer &out, std::string *error,
                             bool worker_request, std::uint64_t worker_generation,
                             std::string_view utterance_language = {});
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
