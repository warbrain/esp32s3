#pragma once

#include "intent/rule.hpp"
#include "input/hotkey.hpp"
#include "base/performance.hpp"
#include "engine/audio_preprocessor.hpp"
#include "engine/hgtcrn_session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace hmi::base { class Logger; }

namespace hmi::app
{
struct VoiceOptions
{
    std::string backend = "auto";
    int threads = 4;
    bool tts_enabled = false;
    bool tts_prewarm = true;
    double preview_interval_seconds = 1.0;
    bool refine = true;
    int maximum_audio_seconds = 30;
    bool vad_enabled = true;
    bool denoise_enabled = false;
    // Default off: on 20 mm 16 kHz input the released masking_on_noisy model
    // raised CER by 12.6 pp (see tools/se/RESULTS-hgtcrn.md in the firmware
    // handoff). Re-enable only after real-device paired CER shows a win.
    bool hgtcrn_enabled = false;
    float vad_threshold = 0.35f;
    std::filesystem::path asr_model;
    std::filesystem::path tts_model;
    std::filesystem::path vad_model;
    std::filesystem::path denoise_model;
    std::filesystem::path hgtcrn_model;
    std::filesystem::path rules_file;
    input::Bindings hotkeys;
    // Where the microphone lives. "local" is WASAPI, "esp32" is the board.
    struct AudioSourceOptions
    {
        std::string source = "local";
        std::string esp32_host = "172.16.30.145";
        int esp32_port = 5000;
        int volume = 70; // Board speaker volume, 0..100.
    } audio_source;
};

enum class VoiceState
{
    Stopped,
    Starting,
    Listening,
    Unavailable, // engine or model missing
    Error
};

struct VoiceEvent
{
    std::int64_t at_ms = 0; // Monotonic request time used for latency measurement.
    std::string transcript;
    float confidence = -1.0f; // Negative when no recognition confidence was reported.
    std::string rule;
    std::string action;
    std::string detail;
    bool ok = false;
    double latency_ms = 0.0;
    bool cancelled = false;
    bool skipped = false;
    std::string speech_text; // Actual TTS payload, which can differ from a command transcript.
    std::uint64_t sequence = 0;
    std::int64_t recorded_at_ms = 0; // Unix epoch milliseconds when the outcome is published; zero before recording.
    double asr_ms = -1.0; // This utterance's preview and final inference time; negative when not applicable.
    double tts_ms = -1.0; // This accepted speech request's synthesis time, excluding playback waits; negative when unavailable.
    double text_ready_ms = -1.0; // Request-to-text-delivered time; negative when no text was injected.
    double speech_end_ms = -1.0; // Request-to-reply-queued time (playback itself runs on the player thread); negative when not spoken.
    std::uint64_t request_id = 0; // Shared by capture, recognition, input and automatic readback.
    double queue_ms = -1.0;
    double rule_ms = -1.0;
    double action_ms = -1.0;
    std::string stop_reason;
    std::string input_delivery; // Empty for legacy outcomes and actions without text delivery.
};

// Read-only view of the voice pipeline, copied once per frame under a lock.
struct VoiceSnapshot
{
    VoiceState state = VoiceState::Stopped;
    bool engine_ready = false;
    bool model_ready = false;
    std::string status; // human readable: "ready", "loading model", "mic denied"...
    float level = 0.0f;
    std::string last_transcript;
    float last_confidence = 0.0f;
    std::string active_rule;
    std::deque<VoiceEvent> recent; // oldest first, capped at 20
    std::string backend;
    int inference_threads = 0;
    bool sdk_openmp_enabled = false;
    base::PerformanceSnapshot performance;
    engine::AudioPreprocessorStatistics audio_frontend;
    engine::HgtcrnStatistics hgtcrn;
    bool transcript_final = true;
    bool refining = false;
    std::size_t chunk_samples = 0;
    std::size_t ring_capacity_samples = 0;
    std::size_t ring_queued_samples = 0;
    std::size_t ring_high_water_samples = 0;
    std::uint64_t partial_revision = 0;
    double preview_ms = 0.0;
    double refine_ms = 0.0;
    double realtime_factor = 0.0;
    double audio_seconds = 0.0;
    double processed_audio_seconds = 0.0;
    bool awaiting_release = false;
    int recording_limit_seconds = 30;
    bool tts_enabled = false;
    bool tts_available = false;
    bool tts_busy = false;
    bool tts_model_ready = false;
    std::string tts_status;
    std::string tts_error;
    std::string tts_toggle_error;
    std::string tts_backend;
    std::size_t tts_segments = 0;
    std::size_t tts_total_segments = 0;
    double tts_generated_audio_seconds = 0.0;
    double tts_first_audio_ms = 0.0;
    double tts_synthesis_ms = 0.0;
    double tts_model_load_ms = 0.0;
    std::size_t tts_cache_entries = 0;
    std::size_t tts_cache_bytes = 0;
    std::uint64_t tts_cache_hits = 0;
    std::uint64_t tts_idle_unloads = 0;
    int tts_idle_timeout_seconds = 60;
    bool capture_active = false;
    std::uint64_t capture_request_id = 0;
    std::size_t captured_frames = 0; // Accepted 16 kHz mono samples for this capture request.
    std::array<float, 64> capture_levels{}; // Completed 90 ms RMS buckets, oldest first and left-padded with zeros.
    float capture_live_level = 0.0f; // RMS of the current incomplete bucket; zero when it is empty.
};

// Orchestrates capture -> ASR -> rules -> actions, plus the TTS feedback path.
// Owns the worker threads; tick() only dispatches on the UI thread.
class VoiceRuntime
{
public:
    explicit VoiceRuntime(base::Logger *logger = nullptr);
    ~VoiceRuntime();
    VoiceRuntime(const VoiceRuntime &) = delete;
    VoiceRuntime &operator=(const VoiceRuntime &) = delete;

    bool start(std::string *error);
    bool start(const VoiceOptions &options, std::string *error);
    void stop();

    bool reload_rules(std::string *error);
    std::string rules_path() const;

    // Queue explicit speech on the action worker. No SDK or playback work runs here.
    bool request_speech(std::string_view text, std::string *error = nullptr);
    // Changes the gate immediately; cancellation never waits for the SDK.
    void set_tts_enabled(bool enabled);

    void tick(); // called once per frame on the UI thread
    VoiceSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
