#include "app/voice_runtime.hpp"

#include "action/actions.hpp"
#include "action/ui_input.hpp"
#include "capture/audio_capture.hpp"
#include "engine/native_client.hpp"
#include "engine/asr_session.hpp"
#include "engine/audio_preprocessor.hpp"
#include "engine/hgtcrn_session.hpp"
#include "engine/tts_session.hpp"
#include "engine/sdk_threads.hpp"
#include "input/hotkey.hpp"
#include "intent/rule_io.hpp"
#include "base/logger.hpp"
#include "base/project_paths.hpp"
#include "app/voice_history.hpp"
#include "audio/playback_diagnostics.hpp"
#include "net/esp32_audio.hpp"

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifndef HMI_AUDIOCPP_SDK_OPENMP_ENABLED
#define HMI_AUDIOCPP_SDK_OPENMP_ENABLED 0
#endif

namespace hmi::app
{
namespace
{
std::int64_t milliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string first_codepoint(const std::string &text)
{
    if (text.empty()) return "empty";
    const auto lead = static_cast<unsigned char>(text.front());
    const std::size_t bytes = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
        (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 0;
    if (bytes == 0 || text.size() < bytes) return "invalid_utf8";
    wchar_t units[2]{};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(bytes), units, 2);
    if (count <= 0) return "invalid_utf8";
    const unsigned int value = count == 2 ? 0x10000u +
        ((static_cast<unsigned int>(units[0]) - 0xd800u) << 10) +
        (static_cast<unsigned int>(units[1]) - 0xdc00u) : static_cast<unsigned int>(units[0]);
    char label[16]{};
    std::snprintf(label, sizeof(label), "U+%04X", value);
    return label;
}

std::string environment(const char *name, std::string fallback)
{
    char *value = nullptr;
    std::size_t size = 0;
    _dupenv_s(&value, &size, name);
    if (value && *value) fallback = value;
    std::free(value);
    return fallback;
}

std::filesystem::path executable_directory()
{
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, 32768);
    return std::filesystem::path(std::wstring(path, count)).parent_path();
}

std::string path_utf8(const std::filesystem::path &path)
{
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

std::filesystem::path rules_file_path(const std::filesystem::path &configured)
{
    wchar_t *value = nullptr;
    std::size_t size = 0;
    _wdupenv_s(&value, &size, L"HMI_RULES");
    const std::unique_ptr<wchar_t, decltype(&std::free)> owned(value, &std::free);
    return value && *value ? std::filesystem::path(value) : configured.empty() ?
        executable_directory() / L"rules/voice.json" : configured;
}

struct AudioRing
{
    std::vector<float> storage;
    std::size_t read = 0;
    std::size_t write = 0;
    std::size_t queued = 0;
    std::size_t high_water = 0;

    void configure(std::size_t capacity)
    {
        storage.assign(capacity, 0.0f);
        clear();
    }

    void clear(bool reset_high_water = true)
    {
        read = write = queued = 0;
        if (reset_high_water) high_water = 0;
    }

    bool push(const float *samples, std::size_t frames)
    {
        if ((!samples && frames != 0) || frames > storage.size() - queued) return false;
        if (frames == 0) return true;
        const auto first = std::min(frames, storage.size() - write);
        std::copy_n(samples, first, storage.data() + write);
        std::copy_n(samples + first, frames - first, storage.data());
        write = (write + frames) % storage.size();
        queued += frames;
        high_water = std::max(high_water, queued);
        return true;
    }

    std::size_t pop(float *samples, std::size_t frames)
    {
        const auto count = std::min(frames, queued);
        if (count == 0) return 0;
        const auto first = std::min(count, storage.size() - read);
        std::copy_n(storage.data() + read, first, samples);
        std::copy_n(storage.data(), count - first, samples + first);
        read = (read + count) % storage.size();
        queued -= count;
        return count;
    }

    std::size_t next_chunk(std::size_t preferred, bool ended) const
    {
        return queued >= preferred ? preferred : ended ? queued : 0;
    }
};

struct Recording
{
    enum class Discard { None, Overrun, Inference, Shortcut } discard = Discard::None;
    std::int64_t started_at_ms = 0;
    std::int64_t released_at = 0;
    action::Target target;
    HWND window = nullptr;
    std::size_t frames = 0;
    float peak = 0.0f;
    double total_sum_squares = 0.0;
    std::size_t total_samples = 0;
    bool started = false;
    bool ended = false;
    bool limit_reached = false;
    std::uint64_t request_id = 0;
    bool dual_complete = true;
    int capture_channels = 0;
    std::array<float, 64> capture_levels{};
    std::size_t capture_level_write = 0;
    std::size_t capture_level_count = 0;
    double capture_sum_squares = 0.0;
    std::size_t capture_bucket_samples = 0;

    bool awaiting_release() const noexcept { return limit_reached && released_at == 0; }

    bool bind_release(std::int64_t when, action::Target released_target)
    {
        if (when <= 0 || released_at != 0) return false;
        released_at = when;
        target = std::move(released_target);
        window = reinterpret_cast<HWND>(target.native_window);
        return true;
    }
};

struct PreviewPublication
{
    std::size_t text_bytes = 0;
    bool nonempty = false;
    bool published = false;
    std::int64_t first_nonempty_ms = -1;
    std::int64_t nonempty_gap_ms = -1;
};

class PreviewPublicationTiming
{
public:
    PreviewPublication observe(const engine::Transcript &event, bool published,
                               std::int64_t started_at_ms, std::int64_t published_at_ms) noexcept
    {
        PreviewPublication result;
        result.text_bytes = event.text.size();
        result.nonempty = !event.text.empty();
        result.published = published && !event.final;
        if (started_at_ms <= 0 || published_at_ms < started_at_ms) return result;
        if (result.published && result.nonempty)
        {
            if (first_nonempty_at_ms_ == 0) first_nonempty_at_ms_ = published_at_ms;
            if (last_nonempty_at_ms_ > 0)
                result.nonempty_gap_ms = std::max<std::int64_t>(0, published_at_ms - last_nonempty_at_ms_);
            last_nonempty_at_ms_ = published_at_ms;
        }
        if (first_nonempty_at_ms_ > 0)
            result.first_nonempty_ms = std::max<std::int64_t>(0, first_nonempty_at_ms_ - started_at_ms);
        return result;
    }

    void reset() noexcept { first_nonempty_at_ms_ = last_nonempty_at_ms_ = 0; }

private:
    std::int64_t first_nonempty_at_ms_ = 0;
    std::int64_t last_nonempty_at_ms_ = 0;
};

VoiceEvent recording_terminal_event(const Recording &recording, std::string reason, std::string detail,
                                     bool cancelled = false, bool skipped = false)
{
    VoiceEvent event;
    event.request_id = recording.request_id;
    event.at_ms = recording.released_at;
    event.confidence = -1.0f;
    event.rule = "è¯­éŸ³è½¬å†™";
    event.detail = std::move(detail);
    event.stop_reason = std::move(reason);
    event.ok = cancelled || skipped;
    event.cancelled = cancelled;
    event.skipped = skipped;
    event.latency_ms = recording.released_at > 0 ?
        static_cast<double>(std::max<std::int64_t>(0, milliseconds() - recording.released_at)) : -1.0;
    return event;
}

std::string recording_discard_reason(Recording::Discard discard)
{
    if (discard == Recording::Discard::Overrun) return "audio_overrun";
    if (discard == Recording::Discard::Shortcut) return "shortcut_unavailable";
    return "asr_failed";
}

/* Final-pass level normalization. Quiet or distant speech reaches the
 * recognizer far below its comfortable range; the retained clip is scaled
 * once for the final pass only. The streaming preview is never modified. */
constexpr float kAsrNormalizeTargetRms = 0.0501f;  /* -26 dBFS */
constexpr float kAsrNormalizeTriggerRms = 0.0200f; /* -34 dBFS */
constexpr float kAsrNormalizeGateRms = 0.0010f;    /* -60 dBFS */
constexpr float kAsrNormalizeMaxGain = 7.94f;      /* +18 dB */

float asr_normalization_gain(const std::vector<float> &audio)
{
    if (audio.empty()) return 1.0f;
    double sum = 0.0;
    for (const float sample : audio) sum += static_cast<double>(sample) * sample;
    const double rms = std::sqrt(sum / static_cast<double>(audio.size()));
    if (rms < static_cast<double>(kAsrNormalizeGateRms) ||
        rms >= static_cast<double>(kAsrNormalizeTriggerRms)) return 1.0f;
    double gain = static_cast<double>(kAsrNormalizeTargetRms) / rms;
    if (gain > static_cast<double>(kAsrNormalizeMaxGain)) gain = kAsrNormalizeMaxGain;
    return static_cast<float>(gain);
}

bool asr_normalization_enabled()
{
    char *value = nullptr;
    std::size_t size = 0;
    bool enabled = true;
    if (_dupenv_s(&value, &size, "HMI_ASR_NORMALIZE") == 0 && value != nullptr)
        enabled = !(value[0] == '0');
    std::free(value);
    return enabled;
}

enum class AudioFeed { Accepted, LimitReached, Failed };

AudioFeed append_recording_audio(Recording &recording, AudioRing &ring,
                                const float *samples, std::size_t frames, std::size_t maximum_frames)
{
    if (recording.discard != Recording::Discard::None) return AudioFeed::Failed;
    const auto remaining = maximum_frames > recording.frames ? maximum_frames - recording.frames : 0;
    const auto accepted = std::min(frames, remaining);
    if (!ring.push(samples, accepted))
    {
        recording.discard = Recording::Discard::Overrun;
        return AudioFeed::Failed;
    }
    recording.frames += accepted;
    for (std::size_t index = 0; index < accepted; ++index)
    {
        recording.peak = std::max(recording.peak, std::abs(samples[index]));
        const double sample = static_cast<double>(samples[index]);
        recording.total_sum_squares += sample * sample;
        ++recording.total_samples;
        recording.capture_sum_squares += sample * sample;
        ++recording.capture_bucket_samples;
        if (recording.capture_bucket_samples == 1440)
        {
            recording.capture_levels[recording.capture_level_write] =
                static_cast<float>(std::sqrt(recording.capture_sum_squares / 1440.0));
            recording.capture_level_write = (recording.capture_level_write + 1) % recording.capture_levels.size();
            recording.capture_level_count = std::min(recording.capture_level_count + 1, recording.capture_levels.size());
            recording.capture_sum_squares = 0.0;
            recording.capture_bucket_samples = 0;
        }
    }
    if (recording.frames >= maximum_frames)
    {
        recording.limit_reached = true;
        return AudioFeed::LimitReached;
    }
    return AudioFeed::Accepted;
}

// Protected by the runtime queue lock. Device ownership stays on the capture
// controller; these transitions never perform device or inference operations.
struct CaptureGate
{
    enum class Phase { Idle, Opening, Recording, Processing } phase = Phase::Idle;
    enum class Request { None, Start, Stop, Rejected };
    bool available = false;
    bool toggled = false;
    bool held = false;
    bool requested = false;
    bool stop_requested = false;

    Request update(input::HotkeyEvent event)
    {
        const bool previous_toggle = toggled;
        if (event == input::HotkeyEvent::Toggle) toggled = !toggled;
        if (event == input::HotkeyEvent::HoldBegin) held = true;
        if (event == input::HotkeyEvent::HoldEnd) held = false;
        const bool wanted = toggled || held;
        if (wanted == requested) return Request::None;
        requested = wanted;
        if (!wanted)
        {
            if (phase != Phase::Opening && phase != Phase::Recording) return Request::None;
            stop_requested = true;
            return Request::Stop;
        }
        if (!available || phase != Phase::Idle)
        {
            if (event == input::HotkeyEvent::Toggle)
            {
                toggled = previous_toggle;
                requested = toggled || held;
            }
            return Request::Rejected;
        }
        phase = Phase::Opening;
        stop_requested = false;
        return Request::Start;
    }

    void finish(bool processing = false)
    {
        phase = processing ? Phase::Processing : Phase::Idle;
        stop_requested = false;
    }
};

bool recording_allows_preview(const Recording &recording, const CaptureGate &gate, bool accepting) noexcept
{
    // Release disables new inference before the capture controller finishes closing.
    return !recording.ended && accepting && !gate.stop_requested;
}

void copy_capture_statistics(VoiceSnapshot &out, const Recording &recording, const CaptureGate &gate,
                             bool accepting, bool stopping)
{
    out.capture_active = recording.started && gate.phase == CaptureGate::Phase::Recording && accepting && !stopping &&
        !gate.stop_requested && !recording.ended && !recording.limit_reached && recording.discard == Recording::Discard::None;
    out.capture_request_id = recording.request_id;
    out.captured_frames = recording.frames;
    out.capture_levels.fill(0.0f);
    const auto count = recording.capture_level_count;
    const auto oldest = (recording.capture_level_write + recording.capture_levels.size() - count) %
        recording.capture_levels.size();
    for (std::size_t index = 0; index < count; ++index)
        out.capture_levels[out.capture_levels.size() - count + index] =
            recording.capture_levels[(oldest + index) % recording.capture_levels.size()];
    out.capture_live_level = recording.capture_bucket_samples == 0 ? 0.0f :
        static_cast<float>(std::sqrt(recording.capture_sum_squares / static_cast<double>(recording.capture_bucket_samples)));
}

template<class ResolveTarget>
bool bind_recording_release(Recording &recording, const CaptureGate &gate, CaptureGate::Request request,
                            std::int64_t when, ResolveTarget &&resolve_target)
{
    if (when <= 0 || recording.released_at != 0) return false;
    if (request != CaptureGate::Request::Stop &&
        !(recording.started && recording.awaiting_release() && !gate.requested)) return false;
    return recording.bind_release(when, resolve_target());
}

bool release_pending(const Recording &recording, const CaptureGate &gate, bool stopping) noexcept
{
    return !stopping && gate.available && recording.discard == Recording::Discard::None &&
        recording.awaiting_release();
}

template<class Session>
bool finish_recording(Session &asr, const Recording &recording, engine::Transcript &out, std::string *error)
{
    if (!recording.ended || recording.frames == 0 || recording.discard != Recording::Discard::None)
    {
        if (error) *error = "Recording is not complete or audio was discarded";
        return false;
    }
    return asr.finish(out, error);
}

template<class Session>
bool prepare_recognition(Session &asr, std::string *error)
{
    // Exercise the SDK before opening a real device. Warmup output is discarded
    // here and can never reach the transcript or action queues.
    const std::vector<float> silence(16000, 0.0f);
    std::vector<engine::Transcript> events;
    engine::Transcript ignored;
    if (!asr.begin(error)) return false;
    try
    {
        const bool ready = asr.push(silence.data(), silence.size(), events, error, false) && asr.finish(ignored, error);
        asr.reset();
        return ready;
    }
    catch (...)
    {
        asr.reset();
        throw;
    }
}

template<class Session, class Cancelled>
bool replay_recording_audio(Session &asr, const std::vector<float> &audio, std::size_t chunk,
                            std::uint64_t request_id, std::string *error, Cancelled cancelled)
{
    if (audio.empty() || audio.size() > 480000 || chunk == 0)
    {
        if (error) *error = "invalid full-recording replay buffer";
        return false;
    }
    asr.reset();
    if (cancelled() || !asr.begin(error, request_id)) return false;
    for (std::size_t offset = 0; offset < audio.size(); offset += chunk)
    {
        if (cancelled()) return false;
        std::vector<engine::Transcript> ignored;
        if (!asr.push(audio.data() + offset, std::min(chunk, audio.size() - offset), ignored, error, false)) return false;
    }
    return true;
}

void apply_preview(VoiceSnapshot &view, const engine::Transcript &event)
{
    if (event.final) return;
    view.last_transcript = event.text;
    view.last_confidence = event.confidence;
    view.transcript_final = false;
    ++view.partial_revision;
}

struct Pending
{
    engine::Transcript transcript;
    std::int64_t released_at = 0;
    action::Target target;
    HWND window = nullptr;
    std::uint64_t speech_generation = 0;
    double asr_ms = -1.0;
    std::uint64_t request_id = 0;
};

struct ActionJob
{
    VoiceEvent event;
    std::vector<intent::Action> actions;
    action::Target target;
    HWND window = nullptr;
    std::uint64_t speech_generation = 0;
    bool fallback_dictation = false;
    bool automatic_readback = false;
    std::int64_t queued_at_ms = 0;
    engine::TtsStatistics speech_statistics;
};

action::InputDelivery merge_input_delivery(action::InputDelivery previous, action::InputDelivery next) noexcept
{
    using Delivery = action::InputDelivery;
    if (previous == Delivery::Mismatch || next == Delivery::Mismatch) return Delivery::Mismatch;
    if (previous == Delivery::Unknown || next == Delivery::Unknown) return Delivery::Unknown;
    if ((previous == Delivery::NotSent) != (next == Delivery::NotSent)) return Delivery::Unknown;
    if (previous == Delivery::NotSent) return Delivery::NotSent;
    if (previous == Delivery::DeliveredUnverified || next == Delivery::DeliveredUnverified)
        return Delivery::DeliveredUnverified;
    return Delivery::Verified;
}

void accumulate_input_result(action::InsertResult &total, const action::InsertResult &input, std::size_t prior_attempts)
{
    const bool input_ok = input.ok && input.delivery != action::InputDelivery::Mismatch;
    if (prior_attempts == 0)
    {
        total = input;
        total.ok = input_ok;
        return;
    }
    const auto merged = merge_input_delivery(total.delivery, input.delivery);
    if (merged != total.delivery)
    {
        if (merged == input.delivery)
        {
            total.verification_reason = input.verification_reason;
            total.detail = input.detail;
            total.path = input.path;
        }
        else total.verification_reason = "multiple text actions have different delivery outcomes";
    }
    const auto accumulate = [](double &sum, double value) {
        if (std::isfinite(value) && value >= 0.0) sum = std::max(0.0, sum) + value;
    };
    accumulate(total.total_ms, input.total_ms);
    accumulate(total.uia_ms, input.uia_ms);
    accumulate(total.uia_worker_ms, input.uia_worker_ms);
    accumulate(total.send_input_ms, input.send_input_ms);
    accumulate(total.clipboard_ms, input.clipboard_ms);
    const auto unknown = [](double value) { return !std::isfinite(value) || value < 0.0; };
    const bool before_unknown = (total.verification_attempted && unknown(total.verification_before_ms)) ||
        (input.verification_attempted && unknown(input.verification_before_ms));
    const bool after_unknown = (total.verification_attempted && unknown(total.verification_after_ms)) ||
        (input.verification_attempted && unknown(input.verification_after_ms));
    accumulate(total.verification_before_ms, input.verification_before_ms);
    accumulate(total.verification_after_ms, input.verification_after_ms);
    if (before_unknown) total.verification_before_ms = -1.0;
    if (after_unknown) total.verification_after_ms = -1.0;
    total.ok = total.ok && input_ok;
    total.delivery = merged;
    total.uia_attempted = total.uia_attempted || input.uia_attempted;
    total.uia_timed_out = total.uia_timed_out || input.uia_timed_out;
    total.send_input_attempted = total.send_input_attempted || input.send_input_attempted;
    total.clipboard_attempted = total.clipboard_attempted || input.clipboard_attempted;
    total.send_input_calls += input.send_input_calls;
    total.verification_attempted = total.verification_attempted || input.verification_attempted;
    total.modifiers |= input.modifiers;
    total.modifiers_observed = total.modifiers_observed && input.modifiers_observed;
    if (total.control_class != input.control_class) total.control_class.clear();
    if (total.control_unicode != input.control_unicode) total.control_unicode = -1;
    if (total.font_name != input.font_name) total.font_name.clear();
}

util::json::Value input_result_fields(const action::InsertResult &input, std::size_t attempts)
{
    using util::json::Value;
    const auto text = [](const std::string &value) { return value.empty() ? Value{} : Value(value); };
    return Value(Value::Object{
        {"attempts", Value(static_cast<double>(attempts))},
        {"delivery", Value(std::string(action::input_delivery_name(input.delivery)))},
        {"total_ms", detail::history_number(input.total_ms)},
        {"uia_ms", detail::history_number(input.uia_ms)},
        {"uia_worker_ms", detail::history_number(input.uia_worker_ms)},
        {"send_input_ms", detail::history_number(input.send_input_ms)},
        {"clipboard_ms", detail::history_number(input.clipboard_ms)},
        {"uia_attempted", Value(input.uia_attempted)}, {"uia_timed_out", Value(input.uia_timed_out)},
        {"send_input_attempted", Value(input.send_input_attempted)},
        {"clipboard_attempted", Value(input.clipboard_attempted)},
        {"send_input_calls", Value(static_cast<double>(input.send_input_calls))},
        {"verification_attempted", Value(input.verification_attempted)},
        {"verification_before_ms", detail::history_number(input.verification_before_ms)},
        {"verification_after_ms", detail::history_number(input.verification_after_ms)},
        {"verification_reason", text(input.verification_reason)}, {"control_class", text(input.control_class)},
        {"control_unicode", input.control_unicode == 0 || input.control_unicode == 1 ? Value(input.control_unicode != 0) : Value{}},
        {"modifiers", input.modifiers_observed ? Value(static_cast<double>(input.modifiers)) : Value{}},
        {"font_name", text(input.font_name)},
        {"font_evidence", input.font_name.empty() ? Value{} : Value("provider_report_only")}});
}

void remove_pending_speech(std::deque<ActionJob> &jobs, std::vector<VoiceEvent> *removed = nullptr)
{
    for (auto &job : jobs)
    {
        std::erase_if(job.actions, [](const intent::Action &item) { return item.kind == "speak"; });
        if (job.actions.empty() && removed) removed->push_back(std::move(job.event));
    }
    std::erase_if(jobs, [](const ActionJob &job) { return job.actions.empty(); });
}

bool valid_speech_text(std::string_view text)
{
    return !text.empty() && text.size() <= 4096 && text.find('\0') == std::string_view::npos &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0 &&
        !intent::normalize(text).empty();
}

template<class Busy, class Cancelled, class Progress, class Wait>
bool wait_for_speech(Busy busy, Cancelled cancelled, Progress progress, Wait wait)
{
    while (!cancelled() && busy())
    {
        progress();
        wait();
    }
    // Cancellation can finish the engine between polls, making busy() false.
    return cancelled();
}

bool recording_allows_tts_preparation(const Recording &recording, const CaptureGate &gate,
                                     bool accepting, bool enabled, bool requested,
                                     const std::vector<engine::Transcript> &events)
{
    return enabled && !requested && recording.started && !recording.limit_reached &&
        recording.discard == Recording::Discard::None && gate.phase == CaptureGate::Phase::Recording &&
        recording_allows_preview(recording, gate, accepting) &&
        std::any_of(events.begin(), events.end(), [](const engine::Transcript &event) {
            return !event.final && valid_speech_text(event.text);
        });
}

enum class AudioGateDecision { TooShort, Silent, Quiet, Refine, RefineQuiet, NoSpeech, RefineVad, RefineUncertain };
constexpr float kQuietPeakThreshold = 0.003f;

std::size_t current_draft_bytes(const VoiceSnapshot &view)
{
    return !view.transcript_final && view.partial_revision > 0 && valid_speech_text(view.last_transcript)
        ? view.last_transcript.size() : 0;
}

AudioGateDecision recording_audio_decision(const Recording &recording, const VoiceSnapshot &view,
                                         std::size_t *draft_bytes = nullptr,
                                         const engine::AudioPreprocessorStatistics *frontend = nullptr)
{
    const auto bytes = current_draft_bytes(view);
    if (draft_bytes) *draft_bytes = bytes;
    if (recording.frames < 1600) return AudioGateDecision::TooShort;
    if (!std::isfinite(recording.peak) || recording.peak <= 0.0f) return AudioGateDecision::Silent;
    const bool vad_valid = frontend && frontend->ready && frontend->vad_enabled && !frontend->failed &&
        !frontend->raw_fallback && frontend->finished;
    if (vad_valid && bytes == 0 && !frontend->speech_seen && frontend->input_frames >= 8000 &&
        frontend->vad_chunks >= 16 && frontend->uncertain_chunks == 0)
        return AudioGateDecision::NoSpeech;
    if (recording.peak >= kQuietPeakThreshold) return AudioGateDecision::Refine;
    if (bytes > 0) return AudioGateDecision::RefineQuiet;
    if (vad_valid && frontend->speech_seen) return AudioGateDecision::RefineVad;
    // Ambiguous and short detector windows must not discard quiet speech.
    if (vad_valid && (frontend->uncertain_chunks > 0 || frontend->vad_chunks < 16))
        return AudioGateDecision::RefineUncertain;
    return AudioGateDecision::Quiet;
}

bool audio_requires_refinement(AudioGateDecision decision) noexcept
{
    return decision == AudioGateDecision::Refine || decision == AudioGateDecision::RefineQuiet ||
        decision == AudioGateDecision::RefineVad || decision == AudioGateDecision::RefineUncertain;
}

const char *audio_gate_reason(AudioGateDecision decision) noexcept
{
    switch (decision)
    {
    case AudioGateDecision::TooShort: return "too_short";
    case AudioGateDecision::Silent: return "zero_or_invalid_peak";
    case AudioGateDecision::Quiet: return "quiet_without_current_draft";
    case AudioGateDecision::RefineQuiet: return "quiet_with_current_draft";
    case AudioGateDecision::NoSpeech: return "vad_confident_no_speech";
    case AudioGateDecision::RefineVad: return "quiet_with_vad_speech";
    case AudioGateDecision::RefineUncertain: return "vad_uncertain_or_short";
    default: return "normal_peak";
    }
}

const char *audio_gate_detail(AudioGateDecision decision) noexcept
{
    switch (decision)
    {
    case AudioGateDecision::TooShort: return "å½•éŸ³æ—¶é—´è¿‡çŸ­ï¼Œæœ¬æ¬¡å·²è·³è¿‡ã€‚";
    case AudioGateDecision::Silent: return "æ²¡æœ‰é‡‡é›†åˆ°æœ‰æ•ˆå£°éŸ³ï¼Œæœ¬æ¬¡å·²è·³è¿‡ã€‚";
    case AudioGateDecision::NoSpeech: return "æœ¬æ¬¡æ²¡æœ‰æ£€æµ‹åˆ°è¯´è¯ï¼Œå·²è·³è¿‡ã€‚";
    default: return "å£°éŸ³è¿‡è½»ä¸”æ²¡æœ‰æœ‰æ•ˆé¢„è§ˆï¼Œæœ¬æ¬¡å·²è·³è¿‡ã€‚";
    }
}

bool speech_conflicts_with_capture(const VoiceSnapshot &view) noexcept
{
    return view.state == VoiceState::Starting || view.state == VoiceState::Listening ||
        view.refining || view.awaiting_release;
}

bool enqueue_speech(std::deque<ActionJob> &jobs, std::string_view text, std::int64_t now, std::string *error)
{
    if (!valid_speech_text(text))
    {
        if (error) *error = "è¯·è¾“å…¥æœ‰æ•ˆæ–‡å­—ï¼Œä¸èƒ½å…¨ä¸ºç©ºç™½ï¼Œä¸”ä¸è¶…è¿‡4096å­—èŠ‚";
        return false;
    }
    if (jobs.size() >= 16)
    {
        if (error) *error = "æœ—è¯»é˜Ÿåˆ—å·²æ»¡ï¼Œè¯·ç­‰å¾…å½“å‰ä»»åŠ¡ç»“æŸ";
        return false;
    }
    ActionJob job;
    job.event.at_ms = now;
    job.queued_at_ms = now;
    job.event.rule = "è¯­éŸ³è¯•å¬";
    job.event.transcript = text;
    job.actions.push_back({"speak", {{"text", std::string(text)}}});
    jobs.push_back(std::move(job));
    if (error) error->clear();
    return true;
}

ActionJob transcript_job(intent::RuleSet &rules, const Pending &item, std::int64_t now)
{
    ActionJob job;
    job.event.at_ms = item.released_at;
    job.queued_at_ms = now;
    job.event.request_id = item.request_id;
    job.event.transcript = item.transcript.text;
    job.event.confidence = item.transcript.confidence;
    job.event.asr_ms = item.asr_ms;
    job.target = item.target;
    job.window = item.window;
    if (!item.transcript.final)
    {
        job.event.detail = "Draft transcription is display-only";
        return job;
    }
    if (intent::normalize(job.event.transcript).empty()) return job;
    if (const intent::Rule *rule = intent::match(rules, job.event.transcript, job.event.confidence, now))
    {
        job.event.rule = rule->name;
        job.actions = rule->actions;
    }
    else
    {
        // Detect an enabled command independently of its execution gates. A
        // cooled-down or low-confidence command must not become dictated text.
        auto triggers = rules;
        for (auto &candidate : triggers.rules)
        {
            candidate.cooldown_ms = 0;
            candidate.last_fired_ms = 0;
        }
        if (const intent::Rule *suppressed = intent::match(triggers, job.event.transcript, 1.0f, now))
        {
            job.event.rule = suppressed->name;
            job.event.detail = "Command suppressed: confidence threshold or cooldown not satisfied";
            return job;
        }
        job.event.rule = "Dictation";
        job.fallback_dictation = true;
        // Unicode input inserts at the caret without replacing a draft or submitting it.
        job.actions.push_back({"text", {{"text", job.event.transcript}, {"target", "sendinput"}}});
    }
    return job;
}

bool transcript_readback_allowed(const Pending &item, const CaptureGate &gate,
                                 bool enabled, std::uint64_t generation)
{
    return enabled && item.transcript.final && item.speech_generation == generation &&
        gate.phase == CaptureGate::Phase::Idle && !gate.requested;
}

bool enqueue_transcript_actions(std::deque<ActionJob> &jobs, ActionJob job,
                                bool readback_enabled, std::uint64_t generation)
{
    if (job.actions.empty() || jobs.size() >= 16) return false;
    job.speech_generation = generation;
    const bool readback = readback_enabled && job.fallback_dictation &&
        valid_speech_text(job.event.transcript) && jobs.size() < 15;
    ActionJob speech;
    if (readback)
    {
        speech.event.at_ms = job.event.at_ms;
        speech.event.transcript = job.event.transcript;
        speech.event.confidence = job.event.confidence;
        speech.event.asr_ms = job.event.asr_ms;
        speech.event.request_id = job.event.request_id;
        speech.queued_at_ms = job.queued_at_ms;
        speech.event.rule = "è½¬å†™æœ—è¯»";
        speech.target = job.target;
        speech.window = job.window;
        speech.speech_generation = generation;
        speech.automatic_readback = true;
        speech.actions.push_back({"speak", {{"text", job.event.transcript}}});
    }
    // Keep typing and playback in distinct jobs so text latency excludes TTS.
    jobs.push_back(std::move(job));
    if (readback) jobs.push_back(std::move(speech));
    return readback;
}
}

struct VoiceRuntime::Impl
{
    base::Logger *logger = nullptr;
    VoiceOptions options;
    mutable std::mutex state_mutex;
    VoiceSnapshot view;
    std::uint64_t event_sequence = 0; // Protected by state_mutex.
    std::atomic<std::uint64_t> next_request_id{1};
    std::atomic<std::uint64_t> active_request_id{0};
    std::atomic<unsigned> speech_cancel_reason{0}; // 1=capture, 2=switch, 3=shutdown, 4=replacement.
    std::string run_id = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    bool history_restored = false;
    capture::CaptureStatistics observed_capture;
    intent::RuleSet rules;
    std::filesystem::path rule_path;
    std::filesystem::file_time_type rule_time{};
    std::filesystem::file_time_type last_rule_attempt{};
    std::int64_t next_reload = 0;
    std::mutex queue_mutex;
    std::condition_variable queue_ready;
    std::condition_variable capture_ready;
    std::deque<Pending> transcripts;
    CaptureGate gate;
    Recording recording;
    AudioRing ring;
    AudioRing stereo_ring;
    std::string ready_status;
    std::atomic<bool> stopping{false};
    std::atomic<bool> accepting{false};
    std::atomic<bool> hotkeys_expected{false};
    std::atomic<bool> tts_enabled{false};
    std::atomic<std::uint64_t> speech_generation{0};
    std::thread pipeline;
    std::thread capture_controller;
    std::mutex action_mutex;
    std::condition_variable action_ready;
    std::deque<ActionJob> jobs;
    std::thread executor;
    capture::AudioCapture capture;
    // Spatial board mic (ESP32-S3 bridge) as an alternative capture source.
    std::unique_ptr<net::Esp32Audio> board_audio;
    input::Hotkeys hotkeys;
    action::TextInput text_input;
    engine::TtsSession speech_session;
    base::PerformanceSampler performance_sampler;

    // One long-lived link: the board keeps its peer registration, so TTS can
    // play back after the microphone has been released.
    bool ensure_board(std::string *error)
    {
        if (board_audio) return board_audio->running();
        net::AudioLinkConfig board_config;
        board_config.host = options.audio_source.esp32_host;
        board_config.port = options.audio_source.esp32_port;
        board_audio = std::make_unique<net::Esp32Audio>();
        const bool opened = board_audio->start(board_config, [this](const float *mono, std::size_t frames,
                                                       std::int64_t) {
            audio(mono, frames, nullptr, 1);
        }, [this](const char *text, std::size_t size) {
            const std::string_view command(text, size);
            if (command == "ptt_down") hotkey(input::HotkeyEvent::HoldBegin);
            else if (command == "ptt_up") hotkey(input::HotkeyEvent::HoldEnd);
            else if (command == "ptt_toggle") hotkey(input::HotkeyEvent::Toggle);
        }, error);
        if (opened) board_audio->set_volume(options.audio_source.volume);
        return opened;
    }

    void status(VoiceState state, std::string text, std::string_view log_text = {})
    {
        bool changed = false;
        {
            std::lock_guard lock(state_mutex);
            changed = view.state != state || view.status != text;
            view.state = state;
            view.status = text;
        }
        if (changed && logger) logger->write_repeated(state == VoiceState::Error || state == VoiceState::Unavailable ?
            base::LogLevel::Error : base::LogLevel::Info, "voice", "request_id=" +
            std::to_string(active_request_id.load()) + " status=" +
            std::string(log_text.empty() ? std::string_view(text) : log_text));
    }

    void record(VoiceEvent event, const engine::TtsStatistics *speech = nullptr,
                const action::InsertResult *input_result = nullptr, std::size_t input_attempts = 0)
    {
        detail::normalize_voice_outcome(event);
        event.recorded_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        { std::lock_guard lock(state_mutex); event.sequence = ++event_sequence; }
        if (logger)
        {
            try
            {
                using util::json::Value;
                auto fields = detail::voice_event_fields(event, run_id);
                if (input_result)
                {
                    fields.emplace("input", input_result_fields(*input_result, input_attempts));
                }
                if (speech && event.tts_ms >= 0.0 && speech->request_id == event.request_id)
                {
                    const auto &play = speech->playback;
                    fields.emplace("tts", Value(Value::Object{
                        {"backend", Value(speech->backend)},
                        {"normal_sampling", Value(speech->sampling_enabled)},
                        {"sampling_fallbacks", Value(static_cast<double>(speech->sampling_fallbacks))},
                        {"statistics_final", Value(!speech->pipeline_finish_reason.empty())},
                        {"segments", Value(static_cast<double>(speech->segments))},
                        {"generated_segments", Value(static_cast<double>(speech->generated_segments))},
                        {"audio_seconds", detail::history_number(speech->audio_seconds)},
                        {"first_pcm_ms", speech->generated_segments ? detail::history_number(speech->first_audio_ms) : Value{}},
                        {"first_pcm_origin", Value("speak_accepted")},
                        {"playback_timing_origin", Value("play_stream_started")},
                        {"queue_ms", detail::history_number(speech->queue_ms)},
                        {"model_load_ms", detail::history_number(speech->model_load_ms)},
                        {"producer_wait_ms", detail::history_number(speech->producer_wait_ms)},
                        {"consumer_wait_ms", detail::history_number(speech->consumer_wait_ms)},
                        {"queue_high_water", Value(static_cast<double>(speech->queue_high_water))},
                        {"retries", Value(static_cast<double>(speech->generation_retries))},
                        {"limit_hits", Value(static_cast<double>(speech->limit_hits))},
                        {"recovery_splits", Value(static_cast<double>(speech->recovery_splits))},
                        {"recovery_attempts", Value(static_cast<double>(speech->recovery_attempts))},
                        {"recovery_exhausted", Value(speech->recovery_exhausted)},
                        {"finish_reason", Value(speech->last_finish_reason)},
                        {"pipeline_finish_reason", Value(speech->pipeline_finish_reason)},
                        {"cache_hits", Value(static_cast<double>(speech->utterance_cache_hits))},
                        {"last_budget", Value(static_cast<double>(speech->max_tokens))}}));
                    fields.emplace("playback_final", Value(play.finalized));
                    fields.emplace("playback", audio::detail::playback_fields(play));
                }
                logger->write(event.ok ? base::LogLevel::Info : base::LogLevel::Error, "history",
                    "event=voice_result " + util::json::stringify(Value(std::move(fields))));
            }
            catch (const std::exception &failure)
            {
                logger->write(base::LogLevel::Error, "history", "event=persist_failed request_id=" +
                    std::to_string(event.request_id) + " detail=" + failure.what());
            }
        }
        {
            std::lock_guard lock(state_mutex);
            // An older completed action must never replace the current transcript.
            view.active_rule = event.rule;
            view.recent.push_back(std::move(event));
            while (view.recent.size() > 20) view.recent.pop_front();
        }
    }

    void hotkey(input::HotkeyEvent event)
    {
        CaptureGate::Request request;
        bool released = false;
        bool lease_requested = false;
        bool lease_accepted = false;
        std::vector<VoiceEvent> removed;
        {
            std::scoped_lock lock(action_mutex, queue_mutex);
            if (stopping.load()) return;
            request = gate.update(event);
            if (request == CaptureGate::Request::Start)
            {
                // Invalidate playback before waking the microphone thread.
                speech_cancel_reason.store(1);
                speech_generation.fetch_add(1);
                speech_session.cancel("capture_started");
                remove_pending_speech(jobs, &removed);
                if (tts_enabled.load())
                {
                    lease_requested = true;
                    lease_accepted = speech_session.defer_idle_unload(45);
                }
                recording = {};
                recording.request_id = next_request_id.fetch_add(1);
                active_request_id.store(recording.request_id);
                ring.clear();
                stereo_ring.clear();
            }
            released = bind_recording_release(recording, gate, request, milliseconds(), [] {
                return action::foreground_target();
            });
            if (released) accepting.store(false);
        }
        if (released) queue_ready.notify_one();
        if (request == CaptureGate::Request::Start || request == CaptureGate::Request::Stop)
            capture_ready.notify_one();
        if (logger) logger->write(base::LogLevel::Info, "hotkey", "request_id=" +
            std::to_string(active_request_id.load()) + " event=" +
            (event == input::HotkeyEvent::HoldBegin ? "hold_pressed" :
             event == input::HotkeyEvent::HoldEnd ? "hold_released" : "toggle_pressed"));
        for (auto &cancelled : removed)
        {
            cancelled.ok = cancelled.cancelled = true;
            cancelled.action = "speak";
            cancelled.detail = "Queued speech cancelled before playback";
            cancelled.stop_reason = "capture_started";
            cancelled.latency_ms = static_cast<double>(milliseconds() - cancelled.at_ms);
            record(std::move(cancelled));
        }
        if (lease_requested && logger) logger->write(base::LogLevel::Debug, "tts",
            "request_id=" + std::to_string(active_request_id.load()) +
            " capture_idle_lease_seconds=45 accepted=" + (lease_accepted ? "1" : "0"));
        if (request == CaptureGate::Request::Rejected)
        {
            if (logger) logger->write(base::LogLevel::Warning, "hotkey",
                "recording rejected: engine unavailable or previous utterance is still processing; microphone stays off");
            return;
        }
    }

    void audio(const float *samples, std::size_t frames, const float *interleaved = nullptr, int channels = 1)
    {
        if (!accepting.load() || stopping.load()) return;
        std::lock_guard lock(queue_mutex);
        if (!accepting.load()) return;
        const auto maximum_frames = static_cast<std::size_t>(options.maximum_audio_seconds) * 16000;
        const auto before = recording.frames;
        const auto feed = append_recording_audio(recording, ring, samples, frames, maximum_frames);
        const auto accepted = recording.frames - before;
        recording.capture_channels = channels;
        if (options.hgtcrn_enabled && accepted > 0 && recording.dual_complete)
        {
            if (channels != 2 || !interleaved || !stereo_ring.push(interleaved, accepted * 2))
            {
                // Enhancement is optional; preserve usable mono on a channel
                // change or stereo-buffer failure instead of losing dictation.
                recording.dual_complete = false;
                stereo_ring.clear(false);
            }
        }
        if (feed != AudioFeed::Accepted)
        {
            accepting.store(false);
            gate.stop_requested = true;
            capture_ready.notify_one();
        }
        queue_ready.notify_one();
    }

    void log_capture_statistics(bool final)
    {
        if (!logger) return;
        const auto sample = capture.statistics();
        const bool changed = sample.open_attempts != observed_capture.open_attempts ||
            sample.reconnects != observed_capture.reconnects || sample.discontinuities != observed_capture.discontinuities ||
            sample.timestamp_errors != observed_capture.timestamp_errors || sample.last_error != observed_capture.last_error;
        observed_capture = sample;
        if (!final && !changed) return;
        logger->write(sample.last_error.empty() ? base::LogLevel::Info : base::LogLevel::Warning, "capture",
            "event=" + std::string(final ? "capture_statistics" : "capture_health_changed") +
            " request_id=" + std::to_string(active_request_id.load()) + " endpoint=" + sample.endpoint_id +
            " open_attempts=" + std::to_string(sample.open_attempts) + " reconnects=" + std::to_string(sample.reconnects) +
            " discontinuities=" + std::to_string(sample.discontinuities) +
            " timestamp_errors=" + std::to_string(sample.timestamp_errors) +
            " silent_flag_frames=" + std::to_string(sample.silent_frames) +
            " dropped_partial_frames=" + std::to_string(sample.dropped_partial_frames) +
            " delivered_frames=" + std::to_string(sample.delivered_frames) +
            " delivered_chunks=" + std::to_string(sample.delivered_chunks) +
            (sample.last_error.empty() ? "" : " last_error=" + sample.last_error));
    }

    void control_capture()
    {
        try
        {
            while (!stopping.load())
            {
                bool open = false;
                {
                    std::unique_lock lock(queue_mutex);
                    const bool ready = capture_ready.wait_for(lock, std::chrono::seconds(1), [this] {
                        return stopping.load() || gate.phase == CaptureGate::Phase::Opening ||
                            (gate.phase == CaptureGate::Phase::Recording && gate.stop_requested);
                    });
                    if (!ready) { lock.unlock(); log_capture_statistics(false); continue; }
                    if (stopping.load()) break;
                    open = gate.phase == CaptureGate::Phase::Opening;
                    if (open && gate.stop_requested)
                    {
                        const Recording completed = recording;
                        speech_session.defer_idle_unload(0);
                        recording = {};
                        if (gate.available)
                            status(VoiceState::Stopped, "No audio captured: shortcut released before microphone opened. " + ready_status);
                        gate.finish();
                        lock.unlock();
                        if (completed.request_id != 0)
                            record(recording_terminal_event(completed, "early_release", "æŒ‰é”®å·²æ¾å¼€ï¼Œæœ¬æ¬¡æœªé‡‡é›†å£°éŸ³ã€‚", false, true));
                        continue;
                    }
                    if (open) accepting.store(true);
                }
                if (open)
                {
                    status(VoiceState::Starting, "Opening microphone...");
                    std::string error;
                    capture::Fusion fusion = capture::Fusion::Mean;
                    char *fusion_env = nullptr;
                    std::size_t fusion_env_size = 0;
                    _dupenv_s(&fusion_env, &fusion_env_size, "HMI_CAPTURE_FUSION");
                    const std::unique_ptr<char, decltype(&std::free)> fusion_owner(fusion_env, &std::free);
                    if (fusion_env)
                    {
                        const std::string text(fusion_env);
                        if (text == "best") fusion = capture::Fusion::Best;
                        else if (text == "left") fusion = capture::Fusion::Left;
                        else if (text == "right") fusion = capture::Fusion::Right;
                    }
                    bool started = false;
                    const bool use_board = options.audio_source.source == "esp32";

                    if (use_board)
                    {
                        started = ensure_board(&error);
                        if (started) board_audio->request_capture(true);
                    }
                    else
                    {
                        started = capture.start_blocks([this](const capture::AudioBlock &block) {
                            audio(block.mono, block.frames, block.interleaved, block.channels);
                        }, &error, fusion);
                    }
                    bool released = false;
                    Recording failed;
                    {
                        std::lock_guard lock(queue_mutex);
                        if (started)
                        {
                            gate.phase = CaptureGate::Phase::Recording;
                            recording.started = true;
                            recording.started_at_ms = milliseconds();
                            released = gate.stop_requested;
                            std::lock_guard state_lock(state_mutex);
                            view.last_transcript.clear();
                            view.last_confidence = -1.0f;
                            view.transcript_final = false;
                            view.refining = false;
                            view.awaiting_release = false;
                            view.partial_revision = 0;
                            view.preview_ms = view.refine_ms = view.realtime_factor = view.audio_seconds = 0.0;
                            view.processed_audio_seconds = 0.0;
                            const auto previous_frontend = view.audio_frontend;
                            view.audio_frontend = {};
                            view.audio_frontend.ready = previous_frontend.ready;
                            view.audio_frontend.vad_enabled = previous_frontend.vad_enabled;
                            view.audio_frontend.denoise_enabled = previous_frontend.denoise_enabled;
                            view.audio_frontend.request_id = recording.request_id;
                            const bool enhancement_ready = view.hgtcrn.ready;
                            view.hgtcrn = {};
                            view.hgtcrn.ready = enhancement_ready;
                            view.hgtcrn.request_id = recording.request_id;
                            view.hgtcrn.channels = static_cast<std::uint32_t>(capture.stream_channels());
                        }
                        else
                        {
                            failed = recording;
                            speech_session.defer_idle_unload(0);
                            accepting.store(false);
                            status(VoiceState::Error, error + "; microphone off; release shortcut and retry");
                            gate.finish();
                            recording = {};
                        }
                    }
                    if (!started)
                    {
                        log_capture_statistics(true);
                        if (failed.request_id != 0)
                            record(recording_terminal_event(failed, "mic_open_failed", "éº¦å…‹é£Žæœªèƒ½æ‰“å¼€ï¼Œæœ¬æ¬¡å½•éŸ³æœªå®Œæˆã€‚"));
                        continue;
                    }
                    const auto native = capture.device_format();
                    if (logger) logger->write(base::LogLevel::Info, "capture", "request_id=" +
                        std::to_string(active_request_id.load()) + (use_board
                            ? " capture started from the board (UDP/Opus uplink)"
                            : " microphone opened on shortcut; device_hz=" + std::to_string(native.sample_rate) +
                              " stream_hz=16000 mono_channels=1 capture_channels=" +
                              std::to_string(capture.stream_channels())));
                    if (!released && !stopping.load()) status(VoiceState::Listening, "Listening... Release the shortcut to transcribe.");
                    queue_ready.notify_one();
                    continue;
                }
                accepting.store(false);
                capture.stop();
                if (board_audio) board_audio->request_capture(false);
                log_capture_statistics(true);
                Recording completed;
                {
                    std::lock_guard lock(queue_mutex);
                    recording.ended = true;
                    completed = recording;
                    gate.finish(true);
                    if (!gate.available)
                        status(VoiceState::Error, "Global shortcut service unavailable; microphone off. Restart voice control.");
                    else if (completed.discard == Recording::Discard::Overrun)
                        status(VoiceState::Error, "Microphone off. Audio ring overrun; utterance discarded. Release shortcut and retry.");
                    else if (completed.discard == Recording::Discard::Inference)
                        status(VoiceState::Error, "Microphone off. Recognition failed; utterance discarded.");
                    else if (completed.limit_reached)
                        status(VoiceState::Starting, "å·²è¾¾åˆ°å½•éŸ³æ—¶é™ï¼ŒéŸ³é¢‘å·²ä¿ç•™ï¼Œæ­£åœ¨æ•´ç†ç»ˆç¨¿ã€‚è¯·æ¾å¼€æŒ‰ä½å¿«æ·é”®ï¼Œæˆ–å†æ¬¡åˆ‡æ¢ç›‘å¬ã€‚",
                            "Recording limit reached; audio retained; finalizing transcription; awaiting shortcut release or toggle stop");
                    else
                        status(VoiceState::Starting, "Microphone off. Finalizing transcription... Wait for Ready.");
                }
                if (logger) logger->write(base::LogLevel::Info, "capture", "request_id=" +
                    std::to_string(completed.request_id) + " microphone stopped; frames=" +
                    std::to_string(completed.frames) + " audio_ms=" + std::to_string(completed.frames / 16) +
                    " peak=" + std::to_string(completed.peak) + " rms=" + std::to_string(completed.total_samples ? std::sqrt(completed.total_sum_squares / static_cast<double>(completed.total_samples)) : 0.0) + " rms_dbfs=" + std::to_string(completed.total_samples ? 20.0 * std::log10(std::sqrt(completed.total_sum_squares / static_cast<double>(completed.total_samples)) + 1e-9) : -180.0) + " target_pid=" + std::to_string(completed.target.pid) +
                    " stop_reason=" + (completed.discard != Recording::Discard::None ? recording_discard_reason(completed.discard) :
                        completed.limit_reached ? std::string("duration_limit_retained") : std::string("shortcut_release")));
                queue_ready.notify_one();
            }
        }
        catch (const std::exception &exception) { status(VoiceState::Error, exception.what()); }
        catch (...) { status(VoiceState::Error, "microphone controller failed"); }
        accepting.store(false);
        capture.stop();
        if (board_audio) board_audio->request_capture(false);
        {
            std::lock_guard lock(queue_mutex);
            gate.available = false;
            if (recording.started)
            {
                if (recording.discard == Recording::Discard::None) recording.discard = Recording::Discard::Inference;
                recording.ended = true;
            }
            else gate.finish();
        }
        queue_ready.notify_all();
        if (logger) logger->write(base::LogLevel::Info, "capture", "microphone controller stopped; device closed");
    }

    void update_statistics(const engine::AsrStatistics &statistics)
    {
        std::lock_guard lock(state_mutex);
        view.backend = statistics.backend;
        view.inference_threads = statistics.threads;
        view.sdk_openmp_enabled = HMI_AUDIOCPP_SDK_OPENMP_ENABLED != 0;
        view.preview_ms = statistics.preview_ms;
        view.refine_ms = statistics.refine_ms;
        view.audio_seconds = static_cast<double>(statistics.audio_frames) / 16000.0;
        view.processed_audio_seconds = view.audio_seconds;
        view.realtime_factor = statistics.audio_frames == 0 ? 0.0 :
            (statistics.preview_ms + statistics.refine_ms) / (static_cast<double>(statistics.audio_frames) / 16.0);
    }

    void log_statistics(const char *stage, const engine::AsrStatistics &statistics,
                        base::LogLevel level = base::LogLevel::Info,
                        const PreviewPublication *publication = nullptr)
    {
        if (!logger) return;
        std::size_t queued = 0;
        std::size_t high_water = 0;
        std::size_t capacity = 0;
        {
            std::lock_guard lock(queue_mutex);
            queued = ring.queued;
            high_water = ring.high_water;
            capacity = ring.storage.size();
        }
        std::uint64_t revision = 0;
        std::size_t chunk = 0;
        {
            std::lock_guard lock(state_mutex);
            revision = view.partial_revision;
            chunk = view.chunk_samples;
        }
        const double rtf = statistics.audio_frames == 0 ? 0.0 :
            (statistics.preview_ms + statistics.refine_ms) / (static_cast<double>(statistics.audio_frames) / 16.0);
        std::string publication_fields;
        if (publication)
        {
            const auto duration = [](std::int64_t value) {
                return value >= 0 ? std::to_string(value) : std::string("N/A");
            };
            publication_fields = " preview_text_bytes=" + std::to_string(publication->text_bytes) +
                " preview_nonempty=" + (publication->nonempty ? "true" : "false") +
                " preview_published=" + (publication->published ? "true" : "false") +
                " first_nonempty_preview_ms=" + duration(publication->first_nonempty_ms) +
                " nonempty_preview_gap_ms=" + duration(publication->nonempty_gap_ms) +
                " preview_timing_origin=mic_started";
        }
        logger->write(level, "asr", std::string(stage) + " request_id=" +
            std::to_string(active_request_id.load()) + " backend=" + statistics.backend +
            " threads=" + std::to_string(statistics.threads) +
            " sdk_openmp=" + (HMI_AUDIOCPP_SDK_OPENMP_ENABLED ? "on" : "off") +
            " chunk=" + std::to_string(chunk) + " ring_queued=" + std::to_string(queued) +
            " ring_capacity=" + std::to_string(capacity) + " ring_high_water=" + std::to_string(high_water) +
            " audio_frames=" + std::to_string(statistics.audio_frames) + " revision=" + std::to_string(revision) +
            " preview_ms=" + std::to_string(statistics.preview_ms) + " refine_ms=" + std::to_string(statistics.refine_ms) +
            " rtf=" + std::to_string(rtf) + publication_fields);
    }

    void update_tts_statistics(const engine::TtsStatistics &statistics)
    {
        std::lock_guard lock(state_mutex);
        view.tts_model_ready = statistics.model_ready;
        view.tts_backend = statistics.backend;
        view.tts_segments = statistics.generated_segments;
        view.tts_total_segments = statistics.segments;
        view.tts_generated_audio_seconds = statistics.audio_seconds;
        view.tts_first_audio_ms = statistics.first_audio_ms;
        view.tts_synthesis_ms = statistics.synthesis_ms;
        view.tts_model_load_ms = statistics.model_load_ms;
        view.tts_cache_entries = statistics.cache_entries;
        view.tts_cache_bytes = statistics.cache_bytes;
        view.tts_cache_hits = statistics.cache_hits;
        view.tts_idle_unloads = statistics.idle_unloads;
        view.tts_idle_timeout_seconds = statistics.idle_timeout_seconds;
    }

    bool perform_speech(engine::TtsSession &tts, std::string_view text, std::string_view source,
                        std::uint64_t generation,
                        std::string &speech_error, bool &was_cancelled, VoiceEvent &event, engine::TtsStatistics &out_statistics)
    {
        was_cancelled = false;
        event.tts_ms = -1.0;
        out_statistics = {};
        std::string failure;
        bool cancelled = false;
        unsigned cancellation_reason = 0;
        const auto mark_cancelled = [&] {
            cancelled = true;
            if (cancellation_reason == 0) cancellation_reason = stopping.load() ? 3 : speech_cancel_reason.load();
        };
        {
            std::lock_guard lock(state_mutex);
            if (stopping.load() || !tts_enabled.load() || generation != speech_generation.load())
            { failure = "æœ—è¯»å·²åœæ­¢ï¼šå½•éŸ³å¼€å§‹æˆ–å¼€å…³çŠ¶æ€å·²æ”¹å˜"; mark_cancelled(); }
            else if (!view.tts_available) failure = view.tts_error.empty() ? "è¯­éŸ³åˆæˆæœåŠ¡ä¸å¯ç”¨" : view.tts_error;
            else
            {
                view.tts_busy = true;
                view.tts_error.clear();
                view.tts_status = "æ­£åœ¨å‡†å¤‡å¹¶æ’­æ”¾è¯­éŸ³â€¦";
                view.tts_segments = 0;
                view.tts_total_segments = 0;
                view.tts_generated_audio_seconds = 0.0;
                view.tts_first_audio_ms = view.tts_synthesis_ms = 0.0;
            }
        }
        bool accepted = false;
        try
        {
            if (failure.empty())
            {
                if (stopping.load()) { failure = "Speech cancelled: voice runtime stopped"; mark_cancelled(); }
                else
                {
                    // Serialize gate changes with queueing, never with SDK inference.
                    std::lock_guard lock(action_mutex);
                    if (stopping.load() || !tts_enabled.load() || generation != speech_generation.load())
                    { failure = "æœ—è¯»å·²åœæ­¢ï¼šå½•éŸ³å¼€å§‹æˆ–å¼€å…³çŠ¶æ€å·²æ”¹å˜"; mark_cancelled(); }
                    else if (!tts.speak(text, event.request_id)) failure = tts.last_error();
                    else accepted = true;
                }
                if (accepted)
                {
                    if (logger) logger->write(base::LogLevel::Info, "tts", "event=speech_started request_id=" + std::to_string(event.request_id) + " source=" + std::string(source) + " text_bytes=" +
                        std::to_string(text.size()));
                    bool model_logged = false;
                    const bool interrupted = wait_for_speech([&tts] { return tts.busy(); }, [this, generation] {
                        return stopping.load() || !tts_enabled.load() || generation != speech_generation.load();
                    }, [this, &tts, &model_logged] {
                        const auto statistics = tts.statistics();
                        update_tts_statistics(statistics);
                        if (statistics.model_ready)
                        {
                            { std::lock_guard lock(state_mutex); view.tts_status = "æ­£åœ¨ç”Ÿæˆå¹¶æ’­æ”¾è¯­éŸ³â€¦"; }
                            if (!model_logged && logger) logger->write(base::LogLevel::Debug, "tts",
                                "model ready; backend=" + statistics.backend + " threads=" + std::to_string(options.threads));
                            model_logged = true;
                        }
                    }, [] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); });
                    // The gate already cancelled the engine; never cancel a newer preparation here.
                    if (interrupted) mark_cancelled();
                    failure = stopping.load() ? "Speech cancelled: voice runtime stopped" :
                        cancelled ? "æœ—è¯»å·²åœæ­¢ï¼šå½•éŸ³å¼€å§‹æˆ–å¼€å…³çŠ¶æ€å·²æ”¹å˜" : tts.last_error();
                }
            }
        }
        catch (const std::exception &exception) { failure = exception.what(); }
        catch (...) { failure = "Unexpected speech request failure"; }
        if (!accepted && failure.empty()) failure = "Speech request was not accepted";
        const auto statistics = accepted ? tts.statistics() : engine::TtsStatistics{};
        if (accepted)
        {
            update_tts_statistics(statistics);
            if (std::isfinite(statistics.synthesis_ms) && statistics.synthesis_ms >= 0.0)
                event.tts_ms = statistics.synthesis_ms;
            out_statistics = statistics;
        }
        bool queued = false;
        {
            std::lock_guard lock(action_mutex);
            // Commit the outcome against the same gate used by capture, switch and stop.
            if (stopping.load() || !tts_enabled.load() || generation != speech_generation.load())
            {
                mark_cancelled();
                failure = stopping.load() ? "Speech cancelled: voice runtime stopped" :
                    "æœ—è¯»å·²åœæ­¢ï¼šå½•éŸ³å¼€å§‹æˆ–å¼€å…³çŠ¶æ€å·²æ”¹å˜";
            }
            for (const auto &pending : jobs)
                for (const auto &item : pending.actions)
                    queued = queued || item.kind == "speak";
            std::lock_guard state_lock(state_mutex);
            view.tts_busy = queued && tts_enabled.load() && !stopping.load();
            view.tts_error = cancelled ? std::string{} : failure;
            view.tts_status = !tts_enabled.load() ? "è¯­éŸ³åˆæˆå·²å…³é—­" : cancelled ? "æœ—è¯»å·²åœæ­¢ï¼Œå¯å†æ¬¡è¯•å¬" :
                !failure.empty() ? "è¯­éŸ³åˆæˆå¤±è´¥" : queued ? "è¯­éŸ³å·²æŽ’é˜Ÿ" : "æœ—è¯»å®Œæˆï¼Œå¯å†æ¬¡è¯•å¬";
        }
        if (cancelled)
        {
            event.stop_reason = cancellation_reason == 3 ? "runtime_stopped" : cancellation_reason == 2 ? "tts_disabled" :
                cancellation_reason == 1 ? "capture_started" : "request_replaced";
        }
        else if (!failure.empty())
            event.stop_reason = !accepted ? "request_rejected" :
                !statistics.pipeline_finish_reason.empty() && statistics.pipeline_finish_reason != "completed" ?
                    statistics.pipeline_finish_reason : "generation_or_playback_failed";
        else event.stop_reason = "completed";
        speech_error = failure;
        was_cancelled = cancelled;
        return cancelled || (accepted && failure.empty());
    }

    void execute_jobs(engine::TtsSession &tts)
    {
        std::uint64_t observed_idle_unloads = 0;
        while (!stopping.load())
        {
            ActionJob job;
            {
                std::unique_lock lock(action_mutex);
                action_ready.wait_for(lock, std::chrono::seconds(1), [this] { return stopping.load() || !jobs.empty(); });
                if (stopping.load()) break;
                if (jobs.empty())
                {
                    lock.unlock();
                    const auto statistics = tts.statistics();
                    update_tts_statistics(statistics);
                    if (statistics.idle_unloads != observed_idle_unloads)
                    {
                        observed_idle_unloads = statistics.idle_unloads;
                        {
                            std::lock_guard state_lock(state_mutex);
                            if (tts_enabled.load() && !view.tts_busy && view.tts_error.empty() && !statistics.model_ready)
                                view.tts_status = statistics.cache_entries ?
                                    "é—²ç½®æ¨¡åž‹å·²é‡Šæ”¾ï¼Œå¸¸ç”¨çŸ­å¥å¯ç›´æŽ¥æ’­æ”¾" : "é—²ç½®æ¨¡åž‹å·²é‡Šæ”¾ï¼Œä¸‹æ¬¡ä½¿ç”¨æ—¶åŠ è½½";
                        }
                        if (logger) logger->write(base::LogLevel::Info, "tts", "idle model released; count=" +
                            std::to_string(statistics.idle_unloads) +
                            " idle_timeout_seconds=" + std::to_string(statistics.idle_timeout_seconds) +
                            " model_ready=" + (statistics.model_ready ? "1" : "0") +
                            " cache_entries=" + std::to_string(statistics.cache_entries) +
                            " cache_bytes=" + std::to_string(statistics.cache_bytes));
                    }
                    continue;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            const auto action_started = std::chrono::steady_clock::now();
            job.event.queue_ms = job.queued_at_ms > 0 ?
                static_cast<double>(std::max<std::int64_t>(0, milliseconds() - job.queued_at_ms)) : -1.0;
            action::Context context;
            action::InsertResult inserted;
            action::InsertResult input_totals;
            std::size_t input_attempts = 0;
            std::string routing = "auto";
            std::string speech_error;
            bool speech_cancelled = false;
            context.insert_text = [this, &inserted, &routing, &job, &input_totals, &input_attempts](std::string_view text) {
                inserted = text_input.insert(text, job.target, routing);
                if (inserted.delivery == action::InputDelivery::Mismatch) inserted.ok = false;
                accumulate_input_result(input_totals, inserted, input_attempts);
                ++input_attempts;
                job.event.input_delivery = action::input_delivery_name(input_totals.delivery);
                return inserted.ok;
            };
            context.speak = [this, &tts, &job, &speech_error, &speech_cancelled](std::string_view text) {
                job.event.speech_text = text;
                return perform_speech(tts, text, job.automatic_readback ? std::string_view("asr_final") :
                    std::string_view(job.event.rule), job.speech_generation, speech_error, speech_cancelled,
                    job.event, job.speech_statistics);
            };
            job.event.ok = true;
            job.event.detail = job.target.process + " (PID " + std::to_string(job.target.pid) + ")";
            for (const intent::Action &item : job.actions)
            {
                if (stopping.load())
                { job.event.ok = true; job.event.cancelled = true; job.event.stop_reason = "runtime_stopped";
                  job.event.detail += "; cancelled"; break; }
                if (item.kind == "key" || item.kind == "text")
                {
                    const auto deadline = milliseconds() + 1500;
                    while (!stopping.load() && milliseconds() < deadline &&
                           ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
                            (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                            (GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
                            (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)))
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    const bool stopped = stopping.load();
                    if (stopped || milliseconds() >= deadline || GetForegroundWindow() != job.window ||
                        action::foreground_target().pid != job.target.pid)
                    {
                        job.event.cancelled = stopped;
                        job.event.ok = job.event.cancelled;
                        job.event.stop_reason = stopped ? "runtime_stopped" : "target_or_modifiers_changed";
                        job.event.detail += "; foreground changed or modifiers held; action cancelled";
                        break;
                    }
                }
                action::Result result;
                bool action_cancelled = false;
                if (item.kind == "wait")
                {
                    const auto parameter = item.params.find("ms");
                    int duration = -1;
                    if (parameter != item.params.end())
                    {
                        const auto &text = parameter->second;
                        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), duration);
                        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) duration = -1;
                    }
                    result.via = "wait";
                    if (duration >= 0 && duration <= 10000)
                    {
                        const auto until = milliseconds() + duration;
                        while (!stopping.load() && milliseconds() < until)
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        action_cancelled = stopping.load();
                        result.ok = !action_cancelled;
                        result.detail = result.ok ? "wait completed" : "wait cancelled";
                    }
                    else result.detail = "wait.ms must be an integer in 0..10000";
                }
                else
                {
                    if (item.kind == "text")
                    {
                        const auto target = item.params.find("target");
                        routing = target == item.params.end() ? "auto" : target->second;
                    }
                    result = action::execute(item, context);
                }
                if (item.kind == "text")
                {
                    result.detail = inserted.detail;
                    switch (inserted.path)
                    {
                    case action::InputPath::UiaValue: result.via = "uia:value"; break;
                    case action::InputPath::UiaLegacy: result.via = "uia:legacy"; break;
                    case action::InputPath::SendInput: result.via = "sendinput"; break;
                    default: result.via = "none"; break;
                    }
                }
                if (item.kind == "speak")
                {
                    job.event.cancelled = job.event.cancelled || speech_cancelled;
                    if (result.ok) result.detail = speech_error.empty() ? "speech completed" : speech_error;
                    else if (!speech_error.empty()) result.detail = speech_error;
                }
                if (!job.event.action.empty()) job.event.action += " -> ";
                job.event.action += item.kind;
                job.event.detail += "; " + result.via + ": " + result.detail;
                if (!result.ok)
                {
                    job.event.cancelled = action_cancelled;
                    job.event.skipped = false;
                    job.event.ok = action_cancelled;
                    if (action_cancelled) job.event.stop_reason = "runtime_stopped";
                    else if (item.kind != "speak" || job.event.stop_reason.empty() || job.event.stop_reason == "completed")
                        job.event.stop_reason = "action_failed";
                    break;
                }
            }
            job.event.action_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - action_started).count();
            if (job.event.stop_reason.empty()) job.event.stop_reason = "completed";
            job.event.latency_ms = static_cast<double>(milliseconds() - job.event.at_ms);
            record(std::move(job.event), &job.speech_statistics, input_attempts ? &input_totals : nullptr, input_attempts);
        }
    }

    void update_frontend_statistics(const engine::AudioPreprocessorStatistics &statistics)
    {
        std::lock_guard lock(state_mutex);
        if (statistics.request_id != 0 && statistics.request_id != active_request_id.load()) return;
        view.audio_frontend = statistics;
    }

    void update_hgtcrn_statistics(const engine::HgtcrnStatistics &statistics)
    {
        std::lock_guard lock(state_mutex);
        if (statistics.request_id != 0 && statistics.request_id != active_request_id.load()) return;
        view.hgtcrn = statistics;
    }

    void log_frontend(const char *phase, const engine::AudioPreprocessorStatistics &statistics,
                      std::size_t raw_frames, const std::string &fallback_reason = {})
    {
        if (!logger) return;
        using util::json::Value;
        logger->write(std::string_view(phase) == "fallback" ? base::LogLevel::Warning : base::LogLevel::Info, "audio",
            "event=audio_frontend " + util::json::stringify(Value(Value::Object{
                {"phase", Value(phase)}, {"request_id", Value(static_cast<double>(statistics.request_id))},
                {"ready", Value(statistics.ready)}, {"vad_enabled", Value(statistics.vad_enabled)},
                {"denoise_enabled", Value(statistics.denoise_enabled)}, {"cpu_threads", Value(1.0)},
                {"finished", Value(statistics.finished)}, {"failed", Value(statistics.failed)},
                {"raw_fallback", Value(statistics.raw_fallback)}, {"fallback_reason", Value(fallback_reason)},
                {"raw_frames", Value(static_cast<double>(raw_frames))},
                {"input_frames", Value(static_cast<double>(statistics.input_frames))},
                {"output_frames", Value(static_cast<double>(statistics.output_frames))},
                {"pending_frames", Value(static_cast<double>(statistics.pending_frames))},
                {"denoise_frames", Value(static_cast<double>(statistics.gtcrn_frames))},
                {"vad_chunks", Value(static_cast<double>(statistics.vad_chunks))},
                {"voiced_chunks", Value(static_cast<double>(statistics.voiced_chunks))},
                {"uncertain_chunks", Value(static_cast<double>(statistics.uncertain_chunks))},
                {"voiced_frames", Value(static_cast<double>(statistics.voiced_frames))},
                {"speech_seen", Value(statistics.speech_seen)}, {"speech_active", Value(statistics.speech_active)},
                {"latest_vad_probability", detail::history_number(statistics.latest_vad_probability)},
                {"max_vad_probability", detail::history_number(statistics.max_vad_probability)},
                {"denoise_ms", detail::history_number(statistics.gtcrn_ms)},
                {"vad_ms", detail::history_number(statistics.vad_ms)},
                {"wall_ms", detail::history_number(statistics.wall_ms)}})));
    }

    void log_hgtcrn(const engine::HgtcrnStatistics &statistics, const std::string &reason)
    {
        if (!logger) return;
        using util::json::Value;
        logger->write(statistics.failed ? base::LogLevel::Warning : base::LogLevel::Info, "audio",
            "event=hgtcrn_result " + util::json::stringify(Value(Value::Object{
                {"request_id", Value(static_cast<double>(statistics.request_id))},
                {"mode", Value("full_segment_dual_channel")}, {"cpu_threads", Value(1.0)},
                {"ready", Value(statistics.ready)}, {"succeeded", Value(statistics.succeeded)},
                {"failed", Value(statistics.failed)}, {"reason", Value(reason)},
                {"channels", Value(static_cast<double>(statistics.channels))},
                {"input_frames", Value(static_cast<double>(statistics.input_frames))},
                {"output_frames", Value(static_cast<double>(statistics.output_frames))},
                {"stft_frames", Value(static_cast<double>(statistics.stft_frames))},
                {"wpe_taps", Value(static_cast<double>(statistics.wpe_taps))},
                {"iva_iterations", Value(static_cast<double>(statistics.iva_iterations))},
                {"iva_speech_channel", Value(static_cast<double>(statistics.iva_speech_channel))},
                {"right_padding_frames", Value(static_cast<double>(statistics.right_padding_frames))},
                {"channel_correlation", detail::history_number(statistics.channel_correlation)},
                {"channel_rms_ratio", detail::history_number(statistics.channel_rms_ratio)},
                {"stft_ms", detail::history_number(statistics.stft_ms)},
                {"wpe_ms", detail::history_number(statistics.wpe_ms)},
                {"iva_ms", detail::history_number(statistics.iva_ms)},
                {"network_ms", detail::history_number(statistics.network_ms)},
                {"istft_ms", detail::history_number(statistics.istft_ms)},
                {"total_ms", detail::history_number(statistics.total_ms)}})));
    }

    void consume_recording(engine::AsrSession &asr, engine::AudioPreprocessor &frontend,
                           engine::HgtcrnSession &hgtcrn, std::vector<float> &chunk)
    {
        std::string error;
        const auto request_id = active_request_id.load();
        bool recognized = asr.begin(&error, request_id);
        auto frontend_statistics = frontend.statistics();
        bool using_frontend = frontend.ready() && (frontend_statistics.vad_enabled || frontend_statistics.denoise_enabled);
        std::string fallback_reason;
        if (!frontend.ready() && (options.vad_enabled || options.denoise_enabled))
        {
            frontend_statistics.raw_fallback = true;
            fallback_reason = "startup_unavailable";
        }
        if (using_frontend)
        {
            std::string detail;
            if (!frontend.begin(request_id, &detail))
            {
                using_frontend = false;
                fallback_reason = std::move(detail);
            }
            frontend_statistics = frontend.statistics();
            frontend_statistics.raw_fallback = !using_frontend;
        }
        frontend_statistics.request_id = request_id;
        std::size_t raw_frames = 0;
        std::string frontend_end_phase = "end";
        struct Summary
        {
            Impl &runtime;
            engine::AudioPreprocessorStatistics &statistics;
            std::size_t &frames;
            std::string &reason;
            std::string &phase;
            ~Summary()
            {
                try
                {
                    runtime.update_frontend_statistics(statistics);
                    runtime.log_frontend(phase.c_str(), statistics, frames, reason);
                }
                catch (...) {}
            }
        } summary{*this, frontend_statistics, raw_frames, fallback_reason, frontend_end_phase};
        update_frontend_statistics(frontend_statistics);
        log_frontend("start", frontend_statistics, 0, fallback_reason);
        std::vector<float> raw;
        const bool retain_raw = true;
        if (retain_raw) raw.reserve(static_cast<std::size_t>(options.maximum_audio_seconds) * 16000);
        std::vector<float> dual, dual_chunk(chunk.size() * 2);
        bool dual_complete = options.hgtcrn_enabled;
        if (dual_complete) dual.reserve(static_cast<std::size_t>(options.maximum_audio_seconds) * 32000);
        std::vector<float> processed;
        double previous_preview_ms = 0.0;
        double previous_refine_ms = 0.0;
        std::size_t previous_preview_count = 0;
        bool enhanced_asr_used = false;
        bool enhancement_changed_asr = false;
        const auto recognition_statistics = [&] {
            auto statistics = asr.statistics();
            statistics.preview_ms += previous_preview_ms;
            statistics.refine_ms += previous_refine_ms;
            statistics.preview_count += previous_preview_count;
            return statistics;
        };
        // A failed processor must never leave a mixture of filtered and raw PCM
        // in the cumulative ASR prefix. Replay the bounded original clip once.
        const auto fallback_to_raw = [&](const std::string &reason, bool preserve_draft = false) {
            fallback_reason = reason;
            frontend_statistics = frontend.statistics();
            frontend_statistics.raw_fallback = true;
            using_frontend = false;
            processed.clear();
            previous_preview_ms += asr.statistics().preview_ms;
            previous_preview_count += asr.statistics().preview_count;
            previous_refine_ms += asr.statistics().refine_ms;
            enhanced_asr_used = false;
            const bool replayed = replay_recording_audio(asr, raw, chunk.size(), request_id, &error,
                [this] { return stopping.load(); });
            if (!preserve_draft)
            {
                std::lock_guard lock(state_mutex);
                view.last_transcript.clear();
                view.partial_revision = 0;
                view.transcript_final = false;
            }
            log_frontend("fallback", frontend_statistics, raw_frames, fallback_reason);
            return replayed;
        };
        bool discarded = false;
        bool preparation_requested = false;
        PreviewPublicationTiming publication_timing;
        while (recognized && !stopping.load())
        {
            std::size_t frames = 0;
            bool allow_preview = true;
            std::int64_t started_at_ms = 0;
            {
                std::unique_lock lock(queue_mutex);
                queue_ready.wait(lock, [this, &chunk] {
                    return stopping.load() || recording.discard != Recording::Discard::None || recording.ended ||
                        ring.next_chunk(chunk.size(), false) != 0;
                });
                if (stopping.load()) break;
                if (recording.discard != Recording::Discard::None)
                {
                    discarded = true;
                    break;
                }
                frames = ring.pop(chunk.data(), ring.next_chunk(chunk.size(), recording.ended));
                if (dual_complete && frames > 0)
                {
                    dual_complete = recording.dual_complete && stereo_ring.queued >= frames * 2;
                    if (dual_complete) stereo_ring.pop(dual_chunk.data(), frames * 2);
                }
                allow_preview = recording_allows_preview(recording, gate, accepting.load());
                started_at_ms = recording.started_at_ms;
                if (frames == 0 && recording.ended) break;
            }
            if (frames == 0) continue;
            raw_frames += frames;
            if (retain_raw) raw.insert(raw.end(), chunk.data(), chunk.data() + frames);
            if (dual_complete) dual.insert(dual.end(), dual_chunk.data(), dual_chunk.data() + frames * 2);
            else dual.clear();
            std::vector<engine::Transcript> events;
            if (using_frontend)
            {
                std::string detail;
                const auto previous_voiced_chunks = frontend_statistics.voiced_chunks;
                if (!frontend.push(chunk.data(), frames, processed, &detail))
                    recognized = fallback_to_raw(detail);
                else
                {
                    frontend_statistics = frontend.statistics();
                    // VAD decides whether a new preview is worth computing. It
                    // never removes samples or ends manual microphone capture.
                    allow_preview = allow_preview && (!frontend_statistics.vad_enabled || frontend_statistics.speech_active ||
                        frontend_statistics.voiced_chunks > previous_voiced_chunks);
                    recognized = processed.empty() || asr.push(processed.data(), processed.size(), events, &error, allow_preview);
                }
            }
            else recognized = asr.push(chunk.data(), frames, events, &error, allow_preview);
            update_frontend_statistics(frontend_statistics);
            const auto statistics = recognition_statistics();
            update_statistics(statistics);
            bool publish = false;
            {
                std::lock_guard lock(queue_mutex);
                publish = recording.discard == Recording::Discard::None && !stopping.load();
            }
            if (recognized && publish)
            {
                std::lock_guard lock(state_mutex);
                for (const auto &event : events) apply_preview(view, event);
            }
            std::uint64_t preparation_request_id = 0;
            bool preparation_accepted = false;
            if (recognized && publish && !preparation_requested)
            {
                // Serialize scheduling with shortcut, switch and shutdown cancellation.
                // Only queue work here; model loading stays on the TTS worker.
                std::scoped_lock lock(action_mutex, queue_mutex);
                if (!stopping.load() && recording_allows_tts_preparation(recording, gate,
                        accepting.load(), tts_enabled.load(), preparation_requested, events))
                {
                    preparation_requested = true;
                    preparation_request_id = recording.request_id;
                    preparation_accepted = speech_session.prepare(preparation_request_id);
                }
            }
            if (preparation_request_id != 0 && logger)
                logger->write(base::LogLevel::Info, "tts", "event=prepare_requested request_id=" +
                    std::to_string(preparation_request_id) + " source=asr_preview accepted=" +
                    (preparation_accepted ? "1" : "0"));
            PreviewPublication publication;
            if (!events.empty())
                publication = publication_timing.observe(events.back(), recognized && publish,
                    started_at_ms, milliseconds());
            log_statistics(events.empty() ? "chunk consumed;" : publish && recognized ?
                "preview replaced;" : "preview discarded;", statistics,
                events.empty() ? base::LogLevel::Debug : base::LogLevel::Info,
                events.empty() ? nullptr : &publication);
        }
        if (stopping.load()) { asr.reset(); return; }
        if (recognized && !discarded && using_frontend)
        {
            std::string detail;
            if (!frontend.finish(processed, &detail)) recognized = fallback_to_raw(detail);
            else
            {
                frontend_statistics = frontend.statistics();
                std::vector<engine::Transcript> ignored;
                recognized = processed.empty() || asr.push(processed.data(), processed.size(), ignored, &error, false);
            }
            update_frontend_statistics(frontend_statistics);
            update_statistics(recognition_statistics());
        }
        if (!recognized || discarded)
        {
            accepting.store(false);
            {
                std::lock_guard lock(queue_mutex);
                if (!recognized && recording.discard == Recording::Discard::None)
                    recording.discard = Recording::Discard::Inference;
                gate.stop_requested = true;
            }
            capture_ready.notify_one();
            asr.reset();
            std::unique_lock lock(queue_mutex);
            queue_ready.wait(lock, [this] { return stopping.load() || recording.ended; });
            if (stopping.load()) return;
            const Recording completed = recording;
            const auto reason = recording.discard;
            speech_session.defer_idle_unload(0);
            recording = {};
            ring.clear(false);
            stereo_ring.clear(false);
            {
                std::lock_guard state_lock(state_mutex);
                view.last_transcript.clear();
                view.transcript_final = true;
                view.refining = false;
                view.awaiting_release = false;
            }
            status(VoiceState::Error, reason == Recording::Discard::Overrun ?
                "Microphone off. Audio ring overrun; utterance discarded. Release shortcut and retry." :
                reason == Recording::Discard::Shortcut ?
                "Global shortcut service unavailable; microphone off. Restart voice control." :
                "Microphone off. Transcription failed: " + error);
            gate.finish();
            lock.unlock();
            if (completed.request_id != 0)
                record(recording_terminal_event(completed, recording_discard_reason(reason),
                    reason == Recording::Discard::Overrun ? "å½•éŸ³å¤„ç†æœªèƒ½è·Ÿä¸Šï¼Œæœ¬æ¬¡å£°éŸ³å·²ä¸¢å¼ƒï¼Œè¯·é‡æ–°å½•éŸ³ã€‚" :
                    reason == Recording::Discard::Shortcut ? "å¿«æ·é”®æœåŠ¡å·²åœæ­¢ï¼Œæœ¬æ¬¡å½•éŸ³æœªå®Œæˆã€‚" :
                    "æœ¬æ¬¡è¯­éŸ³æœªèƒ½å®Œæˆè½¬å†™ï¼Œè¯·é‡æ–°å½•éŸ³ã€‚"));
            return;
        }
        Recording completed;
        { std::lock_guard lock(queue_mutex); completed = recording; }
        std::string retained_draft;
        float retained_confidence = -1.0f;
        {
            std::lock_guard lock(state_mutex);
            if (current_draft_bytes(view) > 0)
            {
                retained_draft = view.last_transcript;
                retained_confidence = view.last_confidence;
            }
        }
        Recording gate_recording = completed;
        if (options.hgtcrn_enabled)
        {
            engine::HgtcrnStatistics enhancement;
            enhancement.ready = hgtcrn.ready();
            enhancement.request_id = request_id;
            enhancement.channels = static_cast<std::uint32_t>(completed.capture_channels);
            enhancement.input_frames = raw_frames;
            std::string reason;
            if (stopping.load()) { asr.reset(); return; }
            if (!hgtcrn.ready()) reason = "startup_unavailable_raw_asr";
            else if (!dual_complete || !completed.dual_complete || dual.size() != raw_frames * 2)
                reason = "synchronous_stereo_unavailable_raw_asr";
            else if (raw_frames < 4608) reason = "short_segment_raw_asr";
            else if (std::none_of(dual.begin(), dual.end(), [](float sample) { return sample != 0.0f; }))
                reason = "silent_dual_channel_raw_asr";
            else
            {
                { std::lock_guard lock(state_mutex); view.refining = true; }
                if (logger) logger->write(base::LogLevel::Info, "audio", "event=hgtcrn_started request_id=" +
                    std::to_string(request_id) + " channels=2 frames=" + std::to_string(raw_frames) + " cpu_threads=1");
                std::vector<float> enhanced;
                std::string detail;
                if (stopping.load()) { asr.reset(); return; }
                if (hgtcrn.run(dual.data(), raw_frames, request_id, enhanced, &detail))
                {
                    enhancement = hgtcrn.statistics();
                    if (!stopping.load())
                    {
                        gate_recording.peak = 0.0f;
                        for (const float sample : enhanced)
                            gate_recording.peak = std::max(gate_recording.peak, std::abs(sample));
                        if (gate_recording.peak == 0.0f && (!retained_draft.empty() || frontend_statistics.speech_seen ||
                                completed.peak >= kQuietPeakThreshold))
                        {
                            // A successful DSP call with zero output must not
                            // erase evidence of speech from the original clip.
                            gate_recording = completed;
                            reason = "enhanced_silent_with_raw_speech_raw_asr";
                        }
                        // The raw detector cannot veto speech recovered from
                        // two channels. Re-score the enhanced timeline once.
                        if (reason.empty() && frontend.ready() && options.vad_enabled)
                        {
                            log_frontend("raw_end", frontend_statistics, raw_frames, fallback_reason);
                            frontend_end_phase = "enhanced_end";
                            bool rescored = frontend.begin(request_id, &detail);
                            std::vector<float> ignored_pcm;
                            for (std::size_t offset = 0; rescored && !stopping.load() && offset < enhanced.size(); offset += chunk.size())
                                rescored = frontend.push(enhanced.data() + offset,
                                    std::min(chunk.size(), enhanced.size() - offset), ignored_pcm, &detail);
                            if (rescored && !stopping.load()) rescored = frontend.finish(ignored_pcm, &detail);
                            frontend_statistics = frontend.statistics();
                            if (!rescored)
                            {
                                frontend_statistics.failed = true;
                                fallback_reason = "enhanced_vad_unavailable: " + detail;
                                log_frontend("fallback", frontend_statistics, raw_frames, fallback_reason);
                            }
                            else fallback_reason.clear();
                            update_frontend_statistics(frontend_statistics);
                        }
                        if (reason.empty() && !stopping.load())
                        {
                            previous_preview_ms += asr.statistics().preview_ms;
                            previous_preview_count += asr.statistics().preview_count;
                            previous_refine_ms += asr.statistics().refine_ms;
                            enhancement_changed_asr = true;
                            recognized = replay_recording_audio(asr, enhanced, chunk.size(), request_id, &error,
                                [this] { return stopping.load(); });
                            if (!recognized)
                            {
                                reason = "enhanced_asr_failed_raw_replay: " + error;
                                recognized = fallback_to_raw(reason, true);
                                gate_recording = completed;
                            }
                            else enhanced_asr_used = true;
                        }
                    }
                }
                else
                {
                    enhancement = hgtcrn.statistics();
                    reason = "enhancement_failed_raw_asr: " + detail;
                }
            }
            update_hgtcrn_statistics(enhancement);
            log_hgtcrn(enhancement, reason);
        }
        if (stopping.load()) { asr.reset(); return; }
        AudioGateDecision audio_decision;
        std::size_t draft_bytes = 0;
        {
            // The accepted draft must belong to this recording, not an earlier result.
            std::lock_guard lock(state_mutex);
            audio_decision = recording_audio_decision(gate_recording, view, &draft_bytes, &frontend_statistics);
        }
        const bool refine_audio = audio_requires_refinement(audio_decision);
        if (logger) logger->write(base::LogLevel::Info, "asr", "event=audio_gate request_id=" +
            std::to_string(completed.request_id) + " decision=" + (refine_audio ? "refine" : "skip") +
            " reason=" + audio_gate_reason(audio_decision) + " frames=" + std::to_string(completed.frames) +
            " peak=" + std::to_string(completed.peak) + " threshold=" + std::to_string(kQuietPeakThreshold) +
            " processed_peak=" + std::to_string(gate_recording.peak) +
            " current_draft_bytes=" + std::to_string(draft_bytes));
        if (!refine_audio)
        {
            asr.reset();
            {
                std::lock_guard lock(queue_mutex);
                speech_session.defer_idle_unload(0);
                recording = {};
                ring.clear(false);
                stereo_ring.clear(false);
                {
                    std::lock_guard state_lock(state_mutex);
                    view.last_transcript.clear();
                    view.transcript_final = true;
                    view.refining = false;
                    view.awaiting_release = false;
                }
                if (gate.available) status(VoiceState::Stopped, std::string(audio_gate_detail(audio_decision)) + " " + ready_status);
                gate.finish();
            }
            if (completed.request_id != 0)
                record(recording_terminal_event(completed, "no_audio", audio_gate_detail(audio_decision), false, true));
            return;
        }
        {
            std::lock_guard lock(state_mutex);
            view.refining = true;
        }
        if (stopping.load()) { asr.reset(); return; }
        /* Scale quiet or distant speech towards -26 dBFS for the final pass.
         * The preview stays untouched; a failed replay falls back to the raw
         * clip so recognition can still complete. */
        if (raw.size() == completed.frames && asr_normalization_enabled() && !stopping.load())
        {
            const float normalize_gain = asr_normalization_gain(raw);
            if (normalize_gain > 1.01f)
            {
                std::vector<float> normalized(raw);
                for (float &sample : normalized) sample *= normalize_gain;
                previous_preview_ms += asr.statistics().preview_ms;
                previous_preview_count += asr.statistics().preview_count;
                previous_refine_ms += asr.statistics().refine_ms;
                const bool replayed = replay_recording_audio(asr, normalized, chunk.size(), request_id,
                    &error, [this] { return stopping.load(); });
                if (replayed)
                {
                    if (logger) logger->write(base::LogLevel::Info, "asr",
                        "event=level_normalized request_id=" + std::to_string(request_id) +
                        " gain=" + std::to_string(normalize_gain) +
                        " frames=" + std::to_string(normalized.size()));
                }
                else
                {
                    recognized = fallback_to_raw("level_normalized_replay_failed: " + error, true);
                    if (logger) logger->write(base::LogLevel::Warning, "asr",
                        "event=level_normalized_fallback request_id=" + std::to_string(request_id) +
                        " error=" + error);
                }
            }
        }
        engine::Transcript final;
        recognized = recognized && finish_recording(asr, completed, final, &error);
        if (!stopping.load() && enhanced_asr_used && (!recognized || !valid_speech_text(final.text)))
        {
            const std::string reason = recognized ? "enhanced_final_empty_raw_replay" :
                "enhanced_final_failed_raw_replay: " + error;
            recognized = fallback_to_raw(reason, true);
            if (recognized && !stopping.load()) recognized = finish_recording(asr, completed, final, &error);
        }
        const auto statistics = recognition_statistics();
        update_statistics(statistics);
        log_statistics("final refinement completed;", statistics);
        if (!recognized) asr.reset();
        if (stopping.load()) return;
        std::unique_lock lock(queue_mutex);
        const bool finished = recognized && final.final;
        const bool valid = finished && recording.discard == Recording::Discard::None &&
            (!enhancement_changed_asr || valid_speech_text(final.text));
        const bool retained_after_failure = enhancement_changed_asr && !retained_draft.empty() &&
            (!finished || !valid_speech_text(final.text));
        bool final_queued = false;
        {
            std::lock_guard state_lock(state_mutex);
            view.refining = false;
            view.transcript_final = !retained_after_failure;
            view.last_transcript = retained_after_failure ? retained_draft : finished ? final.text : std::string{};
            view.last_confidence = retained_after_failure ? retained_confidence : finished ? final.confidence : -1.0f;
            view.awaiting_release = valid && !final.text.empty() && recording.awaiting_release();
        }
        if (valid && !final.text.empty())
        {
            if (logger) logger->write(base::LogLevel::Info, "asr", "final accepted text_bytes=" +
                std::to_string(final.text.size()) + " first_codepoint=" + first_codepoint(final.text));
            if (recording.awaiting_release())
            {
                status(VoiceState::Stopped, "ç»ˆç¨¿å·²ä¿ç•™ï¼›è¯·æ¾å¼€æŒ‰ä½å¿«æ·é”®ï¼Œæˆ–å†æ¬¡åˆ‡æ¢ç›‘å¬ä»¥å®Œæˆæœ¬æ¬¡å¤„ç†ã€‚",
                    "Recording limit reached; final text retained; awaiting shortcut release or toggle stop");
                if (logger) logger->write(base::LogLevel::Info, "asr", "final retained; awaiting_shortcut_release=1");
                queue_ready.wait(lock, [this] {
                    return !release_pending(recording, gate, stopping.load());
                });
                { std::lock_guard state_lock(state_mutex); view.awaiting_release = false; }
                if (stopping.load()) return;
                if (!gate.available || recording.discard != Recording::Discard::None)
                {
                    const Recording abandoned = recording;
                    speech_session.defer_idle_unload(0);
                    status(VoiceState::Error, "ç»ˆç¨¿å·²ä¿ç•™ã€‚å¿«æ·é”®æœåŠ¡å¼‚å¸¸ï¼Œæœ¬æ¬¡æ–‡å­—å°šæœªè¾“å…¥ã€‚",
                        "Final text retained; shortcut service unavailable; no text sent");
                    recording = {};
                    ring.clear(false);
                    stereo_ring.clear(false);
                    gate.finish();
                    lock.unlock();
                    if (abandoned.request_id != 0)
                    {
                        auto event = recording_terminal_event(abandoned, "shortcut_unavailable", "å¿«æ·é”®æœåŠ¡å·²åœæ­¢ï¼Œæœ¬æ¬¡æ–‡å­—æœªè¾“å…¥ã€‚");
                        event.transcript = final.text;
                        event.confidence = final.confidence;
                        event.asr_ms = statistics.preview_ms + statistics.refine_ms;
                        record(std::move(event));
                    }
                    return;
                }
            }
            // Read release metadata after finalization: an automatic limit
            // can close the microphone before the user releases the chord.
            transcripts.push_back({std::move(final), recording.released_at, recording.target, recording.window,
                speech_generation.load(), statistics.preview_ms + statistics.refine_ms, completed.request_id});
            final_queued = true;
            if (tts_enabled.load()) speech_session.defer_idle_unload(2);
            if (gate.available) status(VoiceState::Stopped, ready_status);
        }
        else if (retained_after_failure)
            status(VoiceState::Error, "æœ¬æ¬¡æœªèƒ½ç”Ÿæˆå®Œæ•´ç»“æžœï¼Œå·²ä¿ç•™é¢„è§ˆæ–‡å­—ï¼Œè¯·é‡æ–°å½•éŸ³ã€‚");
        else if (!recognized)
            status(VoiceState::Error, "Microphone off. Final transcription failed: " + error);
        else if (gate.available && recording.discard == Recording::Discard::None)
            status(VoiceState::Stopped, "No text recognized. Microphone off; try speaking a longer phrase. " + ready_status);
        const Recording finalized = recording;
        recording = {};
        ring.clear(false);
        stereo_ring.clear(false);
        if (!final_queued) speech_session.defer_idle_unload(0);
        gate.finish();
        lock.unlock();
        if (!final_queued && finalized.request_id != 0)
        {
            const bool no_text = recognized && final.final && finalized.discard == Recording::Discard::None && !retained_after_failure;
            auto event = recording_terminal_event(finalized,
                no_text ? "no_text" : recording_discard_reason(finalized.discard),
                no_text ? "æœ¬æ¬¡æ²¡æœ‰è¯†åˆ«åˆ°æ–‡å­—ï¼Œå·²è·³è¿‡ã€‚" : "æœ¬æ¬¡è¯­éŸ³æœªèƒ½å®Œæˆè½¬å†™ï¼Œè¯·é‡æ–°å½•éŸ³ã€‚", false, no_text);
            event.asr_ms = statistics.preview_ms + statistics.refine_ms;
            if (finished) { event.transcript = final.text; event.confidence = final.confidence; }
            if (retained_after_failure)
            {
                event.stop_reason = "no_final_preview_retained";
                event.detail = "æœ¬æ¬¡æœªèƒ½ç”Ÿæˆå®Œæ•´ç»“æžœï¼Œå·²ä¿ç•™é¢„è§ˆæ–‡å­—ï¼›æ²¡æœ‰è¾“å…¥æ–‡å­—æˆ–æœ—è¯»ã€‚";
                event.transcript = retained_draft;
                event.confidence = retained_confidence;
            }
            record(std::move(event));
        }
    }

    void run()
    {
        const auto worker_started = milliseconds();
        if (logger) logger->write(base::LogLevel::Info, "engine", "voice worker entered; microphone=off");
        engine::Client client;
        engine::AsrSession asr;
        engine::AudioPreprocessor frontend;
        engine::HgtcrnSession hgtcrn;
        auto &tts = speech_session;
        std::thread warmup_worker;
        std::string error;
        const auto com_started = milliseconds();
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (logger) logger->write(base::LogLevel::Info, "engine", "COM initialization completed; com_init_ms=" +
            std::to_string(milliseconds() - com_started) + " result=" + std::to_string(static_cast<long>(com)));
        const auto work = [&] {
            const auto native_started = milliseconds();
            if (logger) logger->write(base::LogLevel::Info, "engine", "native initialization started; worker_elapsed_ms=" +
                std::to_string(native_started - worker_started));
            const bool native_opened = client.open(&error, [this](const engine::SdkInitializationStage &stage) {
                if (!logger) return;
                logger->write(stage.completed && !stage.success ? base::LogLevel::Error : base::LogLevel::Info,
                    "engine", "SDK initialization stage=" + stage.name +
                    " phase=" + (stage.completed ? "completed" : "started") +
                    " stage_ms=" + std::to_string(stage.elapsed_ms) +
                    (stage.completed ? std::string(" success=") + (stage.success ? "1" : "0") : std::string{}) +
                    (stage.detail.empty() ? std::string{} : " detail=" + stage.detail));
                // Persist the entry before a driver call can block this worker.
                logger->flush();
            }, logger);
            if (logger) logger->write(native_opened ? base::LogLevel::Info : base::LogLevel::Error, "engine",
                "native initialization completed; initialization_ms=" + std::to_string(milliseconds() - worker_started) +
                " client_open_ms=" + std::to_string(milliseconds() - native_started) +
                " success=" + (native_opened ? "1" : "0"));
            if (!native_opened) { status(VoiceState::Unavailable, error); return; }
            { std::lock_guard lock(state_mutex); view.engine_ready = true; }
            engine::AsrConfig config;
            config.model_path = environment("HMI_ASR_MODEL", options.asr_model.empty() ? config.model_path : path_utf8(options.asr_model));
            config.backend = environment("HMI_VOICE_BACKEND", options.backend);
            config.threads = options.threads;
            config.preview_interval_seconds = options.preview_interval_seconds;
            config.refine = options.refine;
            config.maximum_audio_seconds = options.maximum_audio_seconds;
            const engine::detail::ScopedSdkThreads sdk_threads(config.threads);
            if (logger) logger->write(base::LogLevel::Info, "engine", "requested_backend=" + config.backend +
                " threads=" + std::to_string(config.threads) + " sdk=" + client.version().build +
                " sdk_openmp_max_threads=" + std::to_string(sdk_threads.maximum_threads()) +
                " openmp_scope=calling_worker_limit");
            if (stopping.load()) return;
            status(VoiceState::Starting, "Loading Qwen3-ASR model...");
            const auto model_started = milliseconds();
            const bool model_opened = asr.open(client, config, &error);
            if (logger) logger->write(model_opened ? base::LogLevel::Info : base::LogLevel::Error, "asr",
                "model load completed; model_load_ms=" + std::to_string(milliseconds() - model_started) +
                " success=" + (model_opened ? "1" : "0"));
            if (!model_opened) { status(VoiceState::Unavailable, error); return; }
            if (stopping.load()) return;
            status(VoiceState::Starting, "Preparing speech recognition; microphone off...");
            const auto preparation_at = milliseconds();
            if (!prepare_recognition(asr, &error))
            {
                if (logger) logger->write(base::LogLevel::Error, "asr", "warmup failed; warmup_ms=" +
                    std::to_string(milliseconds() - preparation_at) + " detail=" + error);
                status(VoiceState::Error, "Speech recognition preparation failed: " + error);
                return;
            }
            if (logger) logger->write(base::LogLevel::Info, "asr", "warmup completed; microphone=off warmup_ms=" +
                std::to_string(milliseconds() - preparation_at));
            update_statistics(asr.statistics());
            engine::AudioPreprocessorConfig frontend_config;
            frontend_config.vad_enabled = options.vad_enabled;
            frontend_config.denoise_enabled = options.denoise_enabled && !options.hgtcrn_enabled;
            frontend_config.vad_threshold = options.vad_threshold;
            frontend_config.maximum_audio_seconds = options.maximum_audio_seconds;
            frontend_config.vad_model = options.vad_model.empty() ?
                base::project_root(executable_directory()) / L"models/frontend/silero_vad_16k.safetensors" : options.vad_model;
            frontend_config.denoise_model = options.denoise_model;
            const auto frontend_started = milliseconds();
            std::string frontend_error;
            const bool frontend_opened = frontend.open(client, frontend_config, &frontend_error);
            update_frontend_statistics(frontend.statistics());
            if (logger) logger->write(frontend_opened ? base::LogLevel::Info : base::LogLevel::Warning, "audio",
                "event=audio_frontend_initialized ready=" + std::string(frontend_opened ? "1" : "0") +
                " vad_enabled=" + (frontend_opened && frontend.statistics().vad_enabled ? "1" : "0") +
                " denoise_enabled=" + (frontend_opened && frontend.statistics().denoise_enabled ? "1" : "0") +
                " cpu_threads=1 initialization_ms=" + std::to_string(milliseconds() - frontend_started) +
                " fallback=" + (frontend_opened ? "none" : "raw_asr") +
                (frontend_error.empty() ? "" : " detail=" + frontend_error));
            if (options.hgtcrn_enabled)
            {
                engine::HgtcrnConfig enhancement_config;
                enhancement_config.model = options.hgtcrn_model.empty() ?
                    base::project_root(executable_directory()) / L"models/frontend/hgtcrn_noisy.safetensors" : options.hgtcrn_model;
                enhancement_config.maximum_audio_seconds = options.maximum_audio_seconds;
                const auto enhancement_started = milliseconds();
                std::string detail;
                const bool enhancement_opened = hgtcrn.open(client, enhancement_config, &detail);
                update_hgtcrn_statistics(hgtcrn.statistics());
                if (logger) logger->write(enhancement_opened ? base::LogLevel::Info : base::LogLevel::Warning, "audio",
                    "event=hgtcrn_initialized ready=" + std::string(enhancement_opened ? "1" : "0") +
                    " mode=full_segment_dual_channel cpu_threads=1 initialization_ms=" +
                    std::to_string(milliseconds() - enhancement_started) + " fallback=" +
                    (enhancement_opened ? "none" : "raw_asr") + (detail.empty() ? "" : " detail=" + detail));
            }
            const auto chunk_samples = std::clamp<std::size_t>(asr.preferred_chunk_samples(), 1, 16000);
            std::vector<float> chunk(chunk_samples);
            { std::lock_guard lock(state_mutex); view.model_ready = true; view.chunk_samples = chunk_samples; }
            log_statistics("session ready;", asr.statistics());
            if (stopping.load()) return;
            engine::TtsConfig tts_config;
            tts_config.model_path = environment("HMI_TTS_MODEL", options.tts_model.empty() ? tts_config.model_path : path_utf8(options.tts_model));
            tts_config.backend = config.backend;
            tts_config.threads = options.threads;
            tts_config.reference_wav = environment("HMI_TTS_REFERENCE_WAV", "");
            tts_config.logger = logger;
            tts_config.run_id = run_id;
            if (options.audio_source.source == "esp32")
            {
                tts_config.playback = [this](const audio::detail::ReadAudio &read, std::string *out_error,
                                             const std::function<bool()> &cancelled) {
                    if (!board_audio && !ensure_board(out_error))
                    {
                        return false;
                    }
                    audio::AudioBuffer buffer;
                    for (;;)
                    {
                        if (cancelled && cancelled()) return true;
                        const auto state = read(buffer, out_error);
                        if (state == audio::detail::StreamRead::Pending)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            continue;
                        }
                        if (state == audio::detail::StreamRead::Finished) return true;
                        if (state == audio::detail::StreamRead::Failed) return false;
                        if (!board_audio->play(buffer.samples.data(), buffer.samples.size(),
                                               buffer.sample_rate, cancelled))
                        {
                            if (out_error) *out_error = "board playback failed";
                            return false;
                        }
                    }
                };
            }
            const bool tts_opened = tts.open(client, tts_config, &error);
            const bool tts_prewarm = tts_enabled.load() &&
                environment("HMI_TTS_PREWARM", options.tts_prewarm ? "1" : "0") != "0";
            {
                std::lock_guard lock(state_mutex);
                view.tts_available = tts_opened;
                view.tts_error = tts_opened ? std::string{} : error;
                view.tts_status = !tts_enabled.load() ? "è¯­éŸ³åˆæˆå·²å…³é—­" : !tts_opened ? "è¯­éŸ³åˆæˆæœåŠ¡ä¸å¯ç”¨" : tts_prewarm ?
                    "æ­£åœ¨é¢„çƒ­è¯­éŸ³åˆæˆæ¨¡åž‹â€¦" : "æŒ‰éœ€å°±ç»ªï¼šå®šç¨¿åŽè‡ªåŠ¨æœ—è¯»ï¼Œä¹Ÿå¯ç‚¹å‡»è¯•å¬";
                view.tts_busy = tts_opened && tts_prewarm;
            }
            if (logger) logger->write(tts_opened ? base::LogLevel::Info : base::LogLevel::Error, "tts",
                "service initialized; available=" + std::string(tts_opened ? "1" : "0") +
                " enabled=" + (tts_enabled.load() ? "1" : "0") +
                " mode=" + (tts_prewarm ? "prewarm" : "on_demand") + " model_ready=0 requested_backend=" +
                tts_config.backend + " threads=" + std::to_string(tts_config.threads) +
                (tts_opened ? "" : " detail=" + error));
            if (tts_opened && tts_prewarm)
            {
                warmup_worker = std::thread([this, &tts] {
                    if (!tts_enabled.load() || stopping.load()) return;
                    audio::AudioBuffer warmup;
                    std::string detail;
                    const auto started = milliseconds();
                    bool warmed = false;
                    if (logger) logger->write(base::LogLevel::Info, "tts", "warmup started; playback=off");
                    try
                    {
                        warmed = tts.synthesize("ä½ å¥½", warmup, &detail);
                    }
                    catch (const std::exception &exception) { detail = exception.what(); }
                    catch (...) { detail = "unexpected speech warmup failure"; }
                    if (!stopping.load())
                    {
                        const auto statistics = tts.statistics();
                        update_tts_statistics(statistics);
                        if (logger) logger->write(warmed ? base::LogLevel::Info : base::LogLevel::Error, "tts",
                            std::string(warmed ? "warmup completed;" : "warmup failed;") +
                            " warmup_ms=" + std::to_string(milliseconds() - started) +
                            " model_ready=" + (statistics.model_ready ? "1" : "0") +
                            " backend=" + statistics.backend + (warmed ? "" : " detail=" + detail));
                        std::lock_guard lock(state_mutex);
                        if (view.tts_status == "æ­£åœ¨é¢„çƒ­è¯­éŸ³åˆæˆæ¨¡åž‹â€¦")
                        {
                            view.tts_busy = false;
                            view.tts_status = warmed ? "è¯­éŸ³åˆæˆæ¨¡åž‹å·²å°±ç»ª" : "é¢„çƒ­å¤±è´¥ï¼Œå¯ç‚¹å‡»è¯•å¬é‡è¯•";
                            view.tts_error = warmed ? std::string{} : detail;
                        }
                    }
                });
            }
            if (stopping.load()) return;
            { std::lock_guard lock(queue_mutex); gate.available = true; }
            if (!hotkeys.start(options.hotkeys, [this](input::HotkeyEvent event) { hotkey(event); }, &error))
            { status(VoiceState::Error, error); return; }
            hotkeys_expected.store(true);
            const std::string hotkey_warning = error;
            executor = std::thread([this, &tts] { execute_jobs(tts); });
            const auto &bindings = hotkeys.bindings();
            ready_status = "Ready: microphone off. Hold " + bindings.hold + " to talk. " +
                (hotkey_warning.empty() ? "Toggle: " + bindings.toggle + "." : hotkey_warning + ".");
            status(VoiceState::Stopped, ready_status);
            capture_controller = std::thread([this] { control_capture(); });
            while (!stopping.load())
            {
                {
                    std::unique_lock lock(queue_mutex);
                    queue_ready.wait(lock, [this] { return stopping.load() || recording.started; });
                    if (stopping.load()) break;
                }
                consume_recording(asr, frontend, hgtcrn, chunk);
            }
        };
        try { work(); }
        catch (const std::exception &exception) { status(VoiceState::Error, exception.what()); }
        catch (...) { status(VoiceState::Error, "voice worker failed"); }
        {
            std::lock_guard lock(action_mutex);
            stopping.store(true);
            tts.cancel("runtime_stopped");
        }
        hotkeys_expected.store(false);
        hotkeys.stop();
        capture_ready.notify_all();
        queue_ready.notify_all();
        action_ready.notify_all();
        if (capture_controller.joinable()) capture_controller.join();
        { std::lock_guard lock(queue_mutex); gate.available = false; }
        if (executor.joinable()) executor.join();
        if (warmup_worker.joinable()) warmup_worker.join();
        tts.stop();
        frontend.close();
        hgtcrn.close();
        asr.close();
        client.close();
        {
            std::lock_guard lock(state_mutex);
            view.engine_ready = false;
            view.model_ready = false;
            view.audio_frontend.ready = false;
            view.hgtcrn.ready = false;
            view.refining = false;
            view.awaiting_release = false;
            view.tts_available = false;
            view.tts_busy = false;
            view.tts_model_ready = false;
            view.tts_backend.clear();
            view.tts_status = "è¯­éŸ³åˆæˆå·²åœæ­¢";
            if (!view.transcript_final)
            {
                view.last_transcript.clear();
                view.transcript_final = true;
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

VoiceRuntime::VoiceRuntime(base::Logger *logger) : impl_(std::make_unique<Impl>()) { impl_->logger = logger; }
VoiceRuntime::~VoiceRuntime() { stop(); }

bool VoiceRuntime::start(std::string *error)
{
    return start(VoiceOptions{}, error);
}

bool VoiceRuntime::start(const VoiceOptions &options, std::string *error)
{
    stop();
    Impl &impl = *impl_;
    impl.options = options;
    if (options.audio_source.source == "esp32")
    {
        // The board must always have a peer: its button events arrive on this
        // link, and TTS playback reuses it after the microphone is released.
        std::string link_error;
        if (!impl.ensure_board(&link_error))
        {
            if (impl.logger) impl.logger->write(base::LogLevel::Warning, "net",
                "board audio unavailable: " + link_error);
        }
        else if (impl.logger)
        {
            impl.logger->write(base::LogLevel::Info, "net",
                "board audio link opened; host=" + options.audio_source.esp32_host);
        }
    }
    impl.tts_enabled.store(options.tts_enabled);
    impl.speech_generation.store(0);
    impl.speech_cancel_reason.store(0);
    if (impl.logger && impl.logger->ready())
    {
        if (!impl.history_restored)
        {
            impl.logger->flush();
            std::deque<VoiceEvent> restored;
            const auto logging = impl.logger->options();
            const auto count = detail::restore_voice_history(logging.file, restored, logging.max_files);
            { std::lock_guard lock(impl.state_mutex);
              impl.view.recent = std::move(restored);
              for (auto &event : impl.view.recent) event.sequence = ++impl.event_sequence; }
            impl.history_restored = true;
            impl.logger->write(base::LogLevel::Info, "history", "event=history_restored retained=" +
                std::to_string(std::min<std::size_t>(count, 20)));
        }
        impl.logger->write(base::LogLevel::Info, "voice", "event=runtime_started run_id=" + impl.run_id +
            " request_ids=per_runtime source_timing=steady_clock history_timing=unix_ms");
    }
    if (options.threads < 1 || options.threads > 256)
    {
        if (error) *error = "voice threads must be 1..256";
        impl.status(VoiceState::Error, error ? *error : "Invalid voice thread count");
        return false;
    }
    if (!std::isfinite(options.preview_interval_seconds) || options.preview_interval_seconds < 0.5 ||
        options.preview_interval_seconds > 30.0 || options.maximum_audio_seconds < 1 || options.maximum_audio_seconds > 30)
    {
        if (error) *error = "voice preview interval must be 0.5..30 seconds and maximum audio must be 1..30 seconds";
        impl.status(VoiceState::Error, error ? *error : "Invalid voice streaming options");
        return false;
    }
    if (!std::isfinite(options.vad_threshold) || options.vad_threshold <= 0.0f || options.vad_threshold >= 1.0f)
    {
        if (error) *error = "voice VAD threshold must be greater than zero and less than one";
        impl.status(VoiceState::Error, error ? *error : "Invalid voice detection threshold");
        return false;
    }
    impl.rule_path = rules_file_path(options.rules_file);
    if (!base::resolve_project_path(impl.rule_path, executable_directory(), impl.rule_path, error))
    {
        impl.status(VoiceState::Error, error ? *error : "Rules must stay inside the project");
        return false;
    }
    if (!reload_rules(error))
    {
        impl.status(VoiceState::Error, error ? *error : "Cannot load rules");
        return false;
    }
    {
        std::lock_guard lock(impl.state_mutex);
        impl.view.recording_limit_seconds = options.maximum_audio_seconds;
        impl.view.awaiting_release = false;
        impl.view.tts_available = impl.view.tts_busy = impl.view.tts_model_ready = false;
        impl.view.tts_enabled = options.tts_enabled;
        impl.view.audio_frontend = {};
        impl.view.hgtcrn = {};
        impl.view.tts_status = options.tts_enabled ? "æ­£åœ¨å‡†å¤‡è¯­éŸ³åˆæˆæœåŠ¡â€¦" : "è¯­éŸ³åˆæˆå·²å…³é—­";
        impl.view.tts_error.clear();
        impl.view.tts_backend.clear();
        impl.view.tts_segments = 0;
        impl.view.tts_first_audio_ms = impl.view.tts_synthesis_ms = 0.0;
    }
    {
        std::lock_guard lock(impl.queue_mutex);
        impl.ring.configure(static_cast<std::size_t>(options.maximum_audio_seconds) * 16000);
        impl.stereo_ring.configure(options.hgtcrn_enabled ? static_cast<std::size_t>(options.maximum_audio_seconds) * 32000 : 0);
    }
    impl.stopping.store(false);
    impl.performance_sampler.start(impl.logger);
    impl.status(VoiceState::Starting, "Starting voice engine...");
    impl.pipeline = std::thread([&impl] { impl.run(); });
    return true;
}

void VoiceRuntime::stop()
{
    Impl &impl = *impl_;
    {
        std::lock_guard lock(impl.action_mutex);
        impl.speech_cancel_reason.store(3);
        impl.stopping.store(true);
        impl.speech_session.cancel("runtime_stopped");
    }
    impl.accepting.store(false);
    impl.queue_ready.notify_all();
    impl.capture_ready.notify_all();
    impl.action_ready.notify_all();
    if (impl.pipeline.joinable()) impl.pipeline.join();
    impl.performance_sampler.stop();
    Recording interrupted;
    std::deque<Pending> interrupted_transcripts;
    { std::lock_guard lock(impl.queue_mutex);
      interrupted_transcripts.swap(impl.transcripts); interrupted = impl.recording;
      impl.recording = {}; impl.ring.clear(false); impl.stereo_ring.clear(false);
      impl.gate = {}; }
    std::deque<ActionJob> abandoned;
    { std::lock_guard lock(impl.action_mutex); abandoned.swap(impl.jobs); }
    const auto has_queued_action = [&abandoned](std::uint64_t request_id) {
        return std::any_of(abandoned.begin(), abandoned.end(), [request_id](const ActionJob &job) {
            return job.event.request_id == request_id;
        });
    };
    if (interrupted.request_id != 0 && !has_queued_action(interrupted.request_id) &&
        std::none_of(interrupted_transcripts.begin(), interrupted_transcripts.end(), [&interrupted](const Pending &item) {
            return item.request_id == interrupted.request_id;
        }))
        impl.record(recording_terminal_event(interrupted, "runtime_stopped", "è¯­éŸ³æœåŠ¡å·²åœæ­¢ï¼Œæœ¬æ¬¡å½•éŸ³å·²å–æ¶ˆã€‚", true));
    std::vector<std::uint64_t> recorded_transcripts;
    for (auto &item : interrupted_transcripts)
    {
        if (item.request_id == 0 || has_queued_action(item.request_id) ||
            std::find(recorded_transcripts.begin(), recorded_transcripts.end(), item.request_id) != recorded_transcripts.end()) continue;
        recorded_transcripts.push_back(item.request_id);
        Recording completed;
        completed.request_id = item.request_id;
        completed.released_at = item.released_at;
        auto event = recording_terminal_event(completed, "runtime_stopped", "è¯­éŸ³æœåŠ¡å·²åœæ­¢ï¼Œæœ¬æ¬¡æ–‡å­—æœªè¾“å…¥ã€‚", true);
        event.transcript = std::move(item.transcript.text);
        event.confidence = item.transcript.confidence;
        event.asr_ms = item.asr_ms;
        impl.record(std::move(event));
    }
    for (auto &job : abandoned)
    {
        job.event.ok = job.event.cancelled = true;
        job.event.stop_reason = "runtime_stopped";
        job.event.detail = "Queued action cancelled during shutdown";
        job.event.latency_ms = static_cast<double>(milliseconds() - job.event.at_ms);
        impl.record(std::move(job.event));
    }
    { std::lock_guard lock(impl.state_mutex);
      impl.view.state = VoiceState::Stopped; impl.view.engine_ready = false; impl.view.model_ready = false;
      impl.view.status = "Voice stopped"; impl.view.refining = false; impl.view.awaiting_release = false;
      impl.view.tts_available = impl.view.tts_busy = impl.view.tts_model_ready = false;
      impl.view.tts_backend.clear(); impl.view.tts_status = "è¯­éŸ³åˆæˆå·²åœæ­¢"; }
}

bool VoiceRuntime::reload_rules(std::string *error)
{
    Impl &impl = *impl_;
    intent::RuleSet loaded;
    if (!intent::load_rules(path_utf8(impl.rule_path), loaded, error))
    {
        std::error_code code;
        if (std::filesystem::exists(impl.rule_path, code)) return false;
        loaded = intent::default_rules();
        loaded.source_path = path_utf8(impl.rule_path);
    }
    // Preserve cooldowns across a successful file reload.
    for (auto &rule : loaded.rules)
        for (const auto &old : impl.rules.rules)
            if (rule.id == old.id) rule.last_fired_ms = old.last_fired_ms;
    impl.rules = std::move(loaded);
    std::error_code code;
    impl.rule_time = std::filesystem::last_write_time(impl.rule_path, code);
    return true;
}

std::string VoiceRuntime::rules_path() const { return path_utf8(impl_->rule_path); }

bool VoiceRuntime::request_speech(std::string_view text, std::string *error)
{
    Impl &impl = *impl_;
    const auto request_id = impl.next_request_id.fetch_add(1);
    std::string failure;
    bool accepted = false;
    if (!valid_speech_text(text))
        failure = "è¯·è¾“å…¥æœ‰æ•ˆæ–‡å­—ï¼Œä¸èƒ½å…¨ä¸ºç©ºç™½ï¼Œä¸”ä¸è¶…è¿‡4096å­—èŠ‚";
    else
    {
        std::scoped_lock lock(impl.action_mutex, impl.queue_mutex);
        if (impl.stopping.load()) failure = "è¯­éŸ³æœåŠ¡å·²åœæ­¢ï¼Œè¯·å…ˆå¯åŠ¨è¯­éŸ³åŠŸèƒ½";
        else
        {
            std::lock_guard state_lock(impl.state_mutex);
            if (!impl.tts_enabled.load()) failure = "è¯­éŸ³åˆæˆå·²å…³é—­ï¼Œè¯·å…ˆæ‰“å¼€æœ—è¯»å¼€å…³";
            else if (!impl.view.tts_available) failure = "è¯­éŸ³åˆæˆæœåŠ¡å°šæœªå°±ç»ªï¼Œè¯·ç¨åŽé‡è¯•";
            else if (impl.gate.phase != CaptureGate::Phase::Idle || impl.gate.requested ||
                     speech_conflicts_with_capture(impl.view))
                failure = "è¯·å…ˆç»“æŸå½•éŸ³ï¼ˆæ¾å¼€å¿«æ·é”®æˆ–å†æ¬¡åˆ‡æ¢ç›‘å¬ï¼‰ï¼Œå†è¿›è¡Œè¯•å¬";
        }
        if (failure.empty()) accepted = enqueue_speech(impl.jobs, text, milliseconds(), &failure);
        if (accepted)
        {
            impl.jobs.back().speech_generation = impl.speech_generation.load();
            impl.jobs.back().event.request_id = request_id;
            std::lock_guard state_lock(impl.state_mutex);
            impl.view.tts_busy = true;
            impl.view.tts_status = "è¯­éŸ³å·²æŽ’é˜Ÿ";
            impl.view.tts_error.clear();
        }
    }
    if (!accepted)
    {
        { std::lock_guard lock(impl.state_mutex); impl.view.tts_error = failure; }
        if (impl.logger) impl.logger->write(base::LogLevel::Warning, "tts",
            "event=speech_rejected request_id=" + std::to_string(request_id) +
            " text_bytes=" + std::to_string(text.size()) + " detail=" + failure);
    }
    else
    {
        if (impl.logger) impl.logger->write(base::LogLevel::Info, "tts",
            "event=speech_queued request_id=" + std::to_string(request_id) +
            " text_bytes=" + std::to_string(text.size()));
        impl.action_ready.notify_one();
    }
    if (error) *error = failure;
    return accepted;
}

void VoiceRuntime::set_tts_enabled(bool enabled)
{
    Impl &impl = *impl_;
    std::vector<VoiceEvent> removed;
    {
        std::scoped_lock lock(impl.action_mutex, impl.queue_mutex);
        const bool previous = impl.tts_enabled.exchange(enabled);
        if (previous == enabled) return;
        if (!enabled)
        {
            impl.speech_cancel_reason.store(2);
            if (previous) impl.speech_generation.fetch_add(1);
            impl.speech_session.cancel("tts_disabled");
            remove_pending_speech(impl.jobs, &removed);
            impl.speech_session.defer_idle_unload(0);
        }
        else if (impl.gate.phase == CaptureGate::Phase::Opening || impl.gate.phase == CaptureGate::Phase::Recording)
            impl.speech_session.defer_idle_unload(45);
        std::lock_guard state_lock(impl.state_mutex);
        impl.view.tts_enabled = enabled;
        impl.view.tts_busy = false;
        impl.view.tts_error.clear();
        impl.view.tts_status = enabled ? "æŒ‰éœ€å°±ç»ªï¼šå®šç¨¿åŽè‡ªåŠ¨æœ—è¯»ï¼Œä¹Ÿå¯ç‚¹å‡»è¯•å¬" : "è¯­éŸ³åˆæˆå·²å…³é—­";
    }
    impl.action_ready.notify_one();
    if (impl.logger) impl.logger->write(base::LogLevel::Info, "tts",
        std::string("switch changed; enabled=") + (enabled ? "1" : "0"));
    for (auto &cancelled : removed)
    {
        cancelled.ok = cancelled.cancelled = true;
        cancelled.action = "speak";
        cancelled.detail = "Queued speech cancelled before playback";
        cancelled.stop_reason = "tts_disabled";
        cancelled.latency_ms = static_cast<double>(milliseconds() - cancelled.at_ms);
        impl.record(std::move(cancelled));
    }
}

void VoiceRuntime::tick()
{
    Impl &impl = *impl_;
    if (!impl.stopping.load() && impl.hotkeys_expected.load() && !impl.hotkeys.running() && impl.hotkeys_expected.exchange(false))
    {
        impl.accepting.store(false);
        {
            std::lock_guard lock(impl.queue_mutex);
            impl.gate.available = false;
            impl.gate.stop_requested = true;
            if (impl.recording.started) impl.recording.discard = Recording::Discard::Shortcut;
        }
        impl.capture_ready.notify_one();
        impl.queue_ready.notify_all();
        impl.status(VoiceState::Error, "Global shortcut service stopped unexpectedly; microphone off. Restart voice control.");
    }
    const auto now = milliseconds();
    if (now >= impl.next_reload)
    {
        impl.next_reload = now + 2000;
        std::error_code code;
        const auto time = std::filesystem::last_write_time(impl.rule_path, code);
        if (!code && time != impl.rule_time && time != impl.last_rule_attempt)
        {
            impl.last_rule_attempt = time;
            std::string error;
            if (!reload_rules(&error)) impl.status(VoiceState::Error, "Rules reload rejected: " + error);
        }
    }
    std::deque<Pending> pending;
    { std::lock_guard lock(impl.queue_mutex); pending.swap(impl.transcripts); }
    for (auto &item : pending)
    {
        const auto rule_started = std::chrono::steady_clock::now();
        ActionJob job = transcript_job(impl.rules, item, now);
        job.event.rule_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rule_started).count();
        if (job.actions.empty())
        {
            if (job.event.detail.empty())
                job.event.detail = job.event.rule.empty() ? "Empty transcription ignored" : "Matching rule has no actions";
            job.event.ok = true;
            job.event.skipped = true;
            job.event.stop_reason = "no_executable_action";
            impl.record(std::move(job.event));
            continue;
        }
        std::scoped_lock lock(impl.action_mutex, impl.queue_mutex);
        job.speech_generation = item.speech_generation;
        if (!impl.tts_enabled.load())
            std::erase_if(job.actions, [](const intent::Action &item) { return item.kind == "speak"; });
        if (job.actions.empty())
        {
            job.event.ok = true;
            job.event.skipped = true;
            job.event.action = "speak";
            job.event.detail = "Speech skipped: TTS switch disabled";
            job.event.stop_reason = "tts_disabled";
            impl.record(std::move(job.event));
            continue;
        }
        if (impl.jobs.size() >= 16)
        {
            job.event.detail = "Action queue full";
            job.event.stop_reason = "action_queue_full";
            impl.record(std::move(job.event));
        }
        else
        {
            if (impl.logger) impl.logger->write(base::LogLevel::Debug, "voice", "request_id=" +
                std::to_string(job.event.request_id) + " dispatch rule=" + job.event.rule +
                " text_bytes=" + std::to_string(job.event.transcript.size()) +
                " first_codepoint=" + first_codepoint(job.event.transcript) +
                " target_pid=" + std::to_string(job.target.pid));
            const auto request_id = job.event.request_id;
            const bool readback = enqueue_transcript_actions(impl.jobs, std::move(job),
                transcript_readback_allowed(item, impl.gate, impl.tts_enabled.load(), impl.speech_generation.load()),
                item.speech_generation);
            if (readback)
            {
                { std::lock_guard state_lock(impl.state_mutex);
                  impl.view.tts_busy = true;
                  impl.view.tts_error.clear();
                  impl.view.tts_status = "è½¬å†™å·²å®šç¨¿ï¼Œæœ—è¯»å·²æŽ’é˜Ÿ"; }
            }
            if (impl.logger) impl.logger->write(base::LogLevel::Info, "voice",
                "event=action_queued request_id=" + std::to_string(request_id) +
                " readback=" + (readback ? "1" : "0"));
            impl.action_ready.notify_one();
        }
    }
}

VoiceSnapshot VoiceRuntime::snapshot() const
{
    Impl &impl = *impl_;
    VoiceSnapshot result;
    {
        // Keep the transcript and recording identity from the same request.
        std::scoped_lock lock(impl.queue_mutex, impl.state_mutex);
        result = impl.view;
        result.ring_capacity_samples = impl.ring.storage.size();
        result.ring_queued_samples = impl.ring.queued;
        result.ring_high_water_samples = impl.ring.high_water;
        if (impl.recording.started) result.audio_seconds = static_cast<double>(impl.recording.frames) / 16000.0;
        copy_capture_statistics(result, impl.recording, impl.gate, impl.accepting.load(), impl.stopping.load());
    }
    result.level = impl.capture.level();
    result.performance = impl.performance_sampler.snapshot();
    return result;
}
}
