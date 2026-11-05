#include "check.hpp"

#include <limits>

// Exercise the production routing decision without starting a worker or sending input.
#define VoiceRuntime DictationDecisionRuntime
#include "../src/app/voice_runtime.cpp"
#undef VoiceRuntime

HMI_TEST(speech_wait_observes_fast_cancellation_that_already_finished_the_engine)
{
    for (const unsigned reason : {1U, 2U, 3U})
    {
        bool enabled = true;
        bool stopping = false;
        bool busy = true;
        std::uint64_t generation = 7;
        unsigned progress_calls = 0;
        unsigned wait_calls = 0;
        const bool interrupted = hmi::app::wait_for_speech([&] { return busy; }, [&] {
            return stopping || !enabled || generation != 7;
        }, [&] { ++progress_calls; }, [&] {
            ++wait_calls;
            if (reason == 1) ++generation;
            else if (reason == 2) { enabled = false; ++generation; }
            else stopping = true;
            // The engine acknowledges cancellation before the next observer poll.
            busy = false;
        });
        CHECK(interrupted && !busy && progress_calls == 1 && wait_calls == 1);
    }
}

HMI_TEST(speech_wait_rechecks_cancellation_when_busy_returns_false)
{
    bool enabled = true;
    std::uint64_t generation = 7;
    unsigned busy_calls = 0;
    unsigned progress_calls = 0;
    unsigned wait_calls = 0;
    const bool interrupted = hmi::app::wait_for_speech([&] {
        if (++busy_calls == 1) return true;
        // An off/on cycle and completed engine cancellation race the busy check.
        enabled = false;
        ++generation;
        enabled = true;
        return false;
    }, [&] { return !enabled || generation != 7; }, [&] { ++progress_calls; }, [&] { ++wait_calls; });
    CHECK(interrupted && enabled && generation == 8);
    CHECK(busy_calls == 2 && progress_calls == 1 && wait_calls == 1);
}

HMI_TEST(speech_wait_distinguishes_completion_and_cancellation_before_the_first_poll)
{
    bool busy = true;
    bool cancelled = false;
    unsigned progress_calls = 0;
    unsigned wait_calls = 0;
    const auto observe = [&] {
        return hmi::app::wait_for_speech([&] { return busy; }, [&] { return cancelled; },
            [&] { ++progress_calls; }, [&] { ++wait_calls; busy = false; });
    };
    CHECK(!observe() && !busy && progress_calls == 1 && wait_calls == 1);
    CHECK(!observe() && progress_calls == 1 && wait_calls == 1);
    busy = true;
    cancelled = true;
    CHECK(observe() && busy && progress_calls == 1 && wait_calls == 1);
}

HMI_TEST(tts_preparation_requires_a_current_draft_and_is_requested_once_per_recording)
{
    hmi::app::Recording recording;
    recording.started = true;
    hmi::app::CaptureGate gate;
    gate.phase = hmi::app::CaptureGate::Phase::Recording;
    const std::vector<hmi::engine::Transcript> events{{"已经识别出的文字", false, -1.0f}};
    CHECK(hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, events));
    CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, true, true, events));
    CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, false, false, events));
    CHECK(hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, events));
}

HMI_TEST(tts_preparation_ignores_empty_invalid_and_final_recognition_events)
{
    hmi::app::Recording recording;
    recording.started = true;
    hmi::app::CaptureGate gate;
    gate.phase = hmi::app::CaptureGate::Phase::Recording;
    CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, {}));
    for (const auto *text : {"", " \t\r\n", "　", "\xFF"})
    {
        const std::vector<hmi::engine::Transcript> events{{text, false, -1.0f}};
        CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, events));
    }
    const std::vector<hmi::engine::Transcript> final{{"结束后的文字不能再触发提前加载", true, -1.0f}};
    CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, final));
}

HMI_TEST(tts_preparation_stops_with_capture_release_limit_or_discard)
{
    hmi::app::Recording recording;
    recording.started = true;
    hmi::app::CaptureGate gate;
    gate.phase = hmi::app::CaptureGate::Phase::Recording;
    const std::vector<hmi::engine::Transcript> events{{"有效预览", false, -1.0f}};
    CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, false, true, false, events));
    gate.stop_requested = true;
    CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, events));
    gate.stop_requested = false;
    gate.phase = hmi::app::CaptureGate::Phase::Opening;
    CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, events));
    gate.phase = hmi::app::CaptureGate::Phase::Recording;
    recording.started = false;
    CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, events));
    recording.started = true;
    recording.ended = true;
    CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, events));
    recording.ended = false;
    recording.limit_reached = true;
    CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, events));
    recording.limit_reached = false;
    for (const auto discard : {hmi::app::Recording::Discard::Overrun,
                              hmi::app::Recording::Discard::Inference, hmi::app::Recording::Discard::Shortcut})
    {
        recording.discard = discard;
        CHECK(!hmi::app::recording_allows_tts_preparation(recording, gate, true, true, false, events));
    }
}

HMI_TEST(recording_audio_gate_refines_quiet_recordings_with_a_current_published_draft)
{
    using Decision = hmi::app::AudioGateDecision;
    hmi::app::VoiceSnapshot view;
    view.transcript_final = false;
    view.partial_revision = 3;
    view.last_transcript = "本次录音已经识别出的内容";
    hmi::app::Recording recording;
    recording.frames = 63840;
    recording.peak = 0.002254f;
    CHECK(hmi::app::recording_audio_decision(recording, view) == Decision::RefineQuiet);
    CHECK(hmi::app::current_draft_bytes(view) == view.last_transcript.size());
    recording.frames = 35040;
    recording.peak = 0.002942f;
    view.partial_revision = 2;
    CHECK(hmi::app::recording_audio_decision(recording, view) == Decision::RefineQuiet);
}

HMI_TEST(recording_audio_gate_does_not_treat_prior_or_empty_text_as_a_current_draft)
{
    using Decision = hmi::app::AudioGateDecision;
    hmi::app::Recording recording;
    recording.frames = 35040;
    recording.peak = 0.002942f;
    hmi::app::VoiceSnapshot view;
    view.partial_revision = 2;
    view.last_transcript = "上一条已经完成的文字";
    CHECK(hmi::app::recording_audio_decision(recording, view) == Decision::Quiet);
    CHECK(hmi::app::current_draft_bytes(view) == 0);
    view.transcript_final = false;
    view.partial_revision = 0;
    CHECK(hmi::app::recording_audio_decision(recording, view) == Decision::Quiet);
    view.partial_revision = 2;
    for (const auto *text : {"", " \t\r\n", "　\t", "\xFF"})
    {
        view.last_transcript = text;
        CHECK(hmi::app::recording_audio_decision(recording, view) == Decision::Quiet);
        CHECK(hmi::app::current_draft_bytes(view) == 0);
    }
}

HMI_TEST(recording_audio_gate_keeps_short_and_zero_audio_rejected_despite_a_draft)
{
    using Decision = hmi::app::AudioGateDecision;
    hmi::app::VoiceSnapshot view;
    view.transcript_final = false;
    view.partial_revision = 1;
    view.last_transcript = "不能让静音幻觉直接进入动作";
    hmi::app::Recording recording;
    recording.frames = 1599;
    recording.peak = 0.1f;
    CHECK(hmi::app::recording_audio_decision(recording, view) == Decision::TooShort);
    recording.frames = 16000;
    for (const float peak : {0.0f, -0.1f, std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity()})
    {
        recording.peak = peak;
        CHECK(hmi::app::recording_audio_decision(recording, view) == Decision::Silent);
    }
}

HMI_TEST(recording_audio_gate_keeps_the_original_normal_peak_boundary)
{
    using Decision = hmi::app::AudioGateDecision;
    hmi::app::Recording recording;
    recording.frames = 1600;
    hmi::app::VoiceSnapshot view;
    for (const float peak : {hmi::app::kQuietPeakThreshold, 0.003521f, 0.1f})
    {
        recording.peak = peak;
        CHECK(hmi::app::recording_audio_decision(recording, view) == Decision::Refine);
    }
    recording.peak = 0.002999f;
    CHECK(hmi::app::recording_audio_decision(recording, view) == Decision::Quiet);
    CHECK(std::string_view(hmi::app::audio_gate_reason(Decision::TooShort)) == "too_short");
    CHECK(std::string_view(hmi::app::audio_gate_reason(Decision::Silent)) == "zero_or_invalid_peak");
    CHECK(std::string_view(hmi::app::audio_gate_reason(Decision::Quiet)) == "quiet_without_current_draft");
}

HMI_TEST(recording_audio_gate_vad_speech_rescues_quiet_input_without_bypassing_basic_validity)
{
    using Decision = hmi::app::AudioGateDecision;
    hmi::app::Recording recording;
    recording.frames = 16000;
    recording.peak = 0.0001f;
    hmi::app::VoiceSnapshot view;
    hmi::engine::AudioPreprocessorStatistics frontend;
    frontend.ready = true;
    frontend.vad_enabled = true;
    frontend.finished = true;
    frontend.input_frames = recording.frames;
    frontend.vad_chunks = 32;
    frontend.voiced_chunks = 1;
    frontend.voiced_frames = 512;
    frontend.speech_seen = true;
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::RefineVad);
    recording.frames = 1599;
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::TooShort);
    recording.frames = 16000;
    for (const float peak : {0.0f, -0.1f, std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity()})
    {
        recording.peak = peak;
        CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::Silent);
    }
}

HMI_TEST(recording_audio_gate_confident_vad_silence_requires_complete_evidence_and_no_current_text)
{
    using Decision = hmi::app::AudioGateDecision;
    hmi::app::Recording recording;
    recording.frames = 16000;
    recording.peak = 0.1f;
    hmi::app::VoiceSnapshot view;
    hmi::engine::AudioPreprocessorStatistics frontend;
    frontend.ready = true;
    frontend.vad_enabled = true;
    frontend.finished = true;
    frontend.input_frames = recording.frames;
    frontend.vad_chunks = 32;
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::NoSpeech);
    view.transcript_final = false;
    view.partial_revision = 1;
    view.last_transcript = "低音量但本次已识别出的完整内容";
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::Refine);
    recording.peak = 0.0001f;
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::RefineQuiet);
    view.transcript_final = true;
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::NoSpeech);
    view.transcript_final = false;
    view.last_transcript = " \t　";
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::NoSpeech);
    view.last_transcript = "\xFF";
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::NoSpeech);
}

HMI_TEST(recording_audio_gate_uncertain_or_short_vad_preserves_quiet_speech_for_final_recognition)
{
    using Decision = hmi::app::AudioGateDecision;
    hmi::app::Recording recording;
    recording.frames = 16000;
    recording.peak = 0.0001f;
    hmi::app::VoiceSnapshot view;
    hmi::engine::AudioPreprocessorStatistics frontend;
    frontend.ready = true;
    frontend.vad_enabled = true;
    frontend.finished = true;
    frontend.input_frames = recording.frames;
    frontend.vad_chunks = 32;
    frontend.uncertain_chunks = 1;
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::RefineUncertain);
    frontend.uncertain_chunks = 0;
    frontend.input_frames = 7999;
    frontend.vad_chunks = 15;
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::RefineUncertain);
    frontend.input_frames = 8000;
    frontend.vad_chunks = 16;
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) == Decision::NoSpeech);
    frontend.finished = false;
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) != Decision::NoSpeech);
}

HMI_TEST(recording_audio_gate_failed_or_bypassed_vad_keeps_the_raw_audio_policy)
{
    hmi::app::Recording recording;
    recording.frames = 16000;
    hmi::app::VoiceSnapshot view;
    hmi::engine::AudioPreprocessorStatistics frontend;
    frontend.ready = true;
    frontend.finished = true;
    frontend.input_frames = recording.frames;
    frontend.vad_chunks = 32;
    for (const unsigned reason : {1u, 2u, 3u})
    {
        frontend.vad_enabled = reason != 1;
        frontend.failed = reason == 2;
        frontend.raw_fallback = reason == 3;
        for (const float peak : {0.0001f, 0.1f})
        {
            recording.peak = peak;
            CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) ==
                hmi::app::recording_audio_decision(recording, view));
        }
    }
}

HMI_TEST(recording_vad_silence_does_not_release_the_capture_gate)
{
    hmi::app::Recording recording;
    recording.started = true;
    recording.frames = 16000;
    recording.peak = 0.0001f;
    hmi::app::CaptureGate gate;
    gate.phase = hmi::app::CaptureGate::Phase::Recording;
    gate.available = true;
    gate.held = true;
    gate.requested = true;
    hmi::app::VoiceSnapshot view;
    hmi::engine::AudioPreprocessorStatistics frontend;
    frontend.ready = true;
    frontend.vad_enabled = true;
    frontend.finished = true;
    frontend.input_frames = recording.frames;
    frontend.vad_chunks = 32;
    CHECK(hmi::app::recording_audio_decision(recording, view, nullptr, &frontend) ==
        hmi::app::AudioGateDecision::NoSpeech);
    CHECK(gate.update(hmi::input::HotkeyEvent::HoldBegin) == hmi::app::CaptureGate::Request::None);
    hmi::app::copy_capture_statistics(view, recording, gate, true, false);
    CHECK(view.capture_active && !recording.ended && !gate.stop_requested);
    CHECK(gate.phase == hmi::app::CaptureGate::Phase::Recording);
    CHECK(gate.update(hmi::input::HotkeyEvent::HoldEnd) == hmi::app::CaptureGate::Request::Stop);
}

namespace
{
struct RecordingReplayFixture
{
    unsigned resets = 0;
    unsigned begins = 0;
    unsigned pushes = 0;
    unsigned fail_push = 0;
    bool begin_ok = true;
    bool preview_requested = false;
    std::uint64_t request_id = 0;
    std::vector<float> audio;
    std::vector<std::size_t> chunks;

    void reset() { ++resets; audio.clear(); chunks.clear(); }
    bool begin(std::string *, std::uint64_t id) { ++begins; request_id = id; return begin_ok; }
    bool push(const float *samples, std::size_t count, std::vector<hmi::engine::Transcript> &,
              std::string *error, bool allow_preview)
    {
        preview_requested = preview_requested || allow_preview;
        if (++pushes == fail_push)
        {
            if (error) *error = "fixture replay push failed";
            return false;
        }
        audio.insert(audio.end(), samples, samples + count);
        chunks.push_back(count);
        return true;
    }
};
}

HMI_TEST(recording_replay_refeeds_the_complete_current_buffer_without_previews)
{
    RecordingReplayFixture session;
    session.audio = {99.0f};
    const std::vector<float> recording{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f};
    std::string error;
    CHECK(hmi::app::replay_recording_audio(session, recording, 3, 71, &error, [] { return false; }));
    CHECK(session.resets == 1 && session.begins == 1 && session.request_id == 71);
    CHECK(session.audio == recording && !session.preview_requested);
    CHECK(session.chunks == std::vector<std::size_t>({3, 3, 1}));
    CHECK(hmi::app::replay_recording_audio(session, recording, 100, 72, &error, [] { return false; }));
    CHECK(session.audio == recording && session.request_id == 72);
    CHECK(session.chunks == std::vector<std::size_t>{recording.size()});
}

HMI_TEST(recording_replay_validates_bounds_and_stops_on_cancellation_or_session_failure)
{
    const std::vector<float> recording(7, 0.125f);
    std::string error;
    RecordingReplayFixture invalid;
    CHECK(!hmi::app::replay_recording_audio(invalid, {}, 3, 1, &error, [] { return false; }));
    CHECK(!hmi::app::replay_recording_audio(invalid, recording, 0, 1, &error, [] { return false; }));
    CHECK(!hmi::app::replay_recording_audio(invalid, std::vector<float>(480001), 3, 1,
        &error, [] { return false; }));
    CHECK(invalid.resets == 0 && invalid.begins == 0 && invalid.pushes == 0);
    RecordingReplayFixture cancelled;
    CHECK(!hmi::app::replay_recording_audio(cancelled, recording, 3, 1, &error, [] { return true; }));
    CHECK(cancelled.resets == 1 && cancelled.begins == 0 && cancelled.pushes == 0);
    unsigned polls = 0;
    CHECK(!hmi::app::replay_recording_audio(cancelled, recording, 3, 2, &error, [&] { return ++polls == 3; }));
    CHECK(cancelled.audio == std::vector<float>(3, 0.125f) && cancelled.pushes == 1);
    RecordingReplayFixture failed;
    failed.fail_push = 2;
    CHECK(!hmi::app::replay_recording_audio(failed, recording, 3, 3, &error, [] { return false; }));
    CHECK(failed.pushes == 2 && failed.audio.size() == 3 && !error.empty());
    failed.begin_ok = false;
    CHECK(!hmi::app::replay_recording_audio(failed, recording, 3, 4, &error, [] { return false; }));
    CHECK(failed.audio.empty() && failed.pushes == 2);
}

HMI_TEST(preview_publication_timing_ignores_empty_and_unpublished_candidates)
{
    hmi::app::PreviewPublicationTiming timing;
    const auto empty = timing.observe({"", false, -1.0f}, true, 1000, 1100);
    CHECK(empty.published && !empty.nonempty && empty.text_bytes == 0);
    CHECK(empty.first_nonempty_ms < 0 && empty.nonempty_gap_ms < 0);
    const auto discarded = timing.observe({"文字", false, -1.0f}, false, 1000, 1200);
    CHECK(!discarded.published && discarded.nonempty && discarded.text_bytes == 6);
    CHECK(discarded.first_nonempty_ms < 0 && discarded.nonempty_gap_ms < 0);
    const auto first = timing.observe({"本次预览", false, -1.0f}, true, 1000, 1400);
    CHECK(first.published && first.nonempty && first.text_bytes == 12);
    CHECK(first.first_nonempty_ms == 400 && first.nonempty_gap_ms < 0);
}

HMI_TEST(preview_publication_timing_measures_only_nonempty_published_gaps)
{
    hmi::app::PreviewPublicationTiming timing;
    CHECK(timing.observe({"首段", false, -1.0f}, true, 1000, 1400).first_nonempty_ms == 400);
    const auto empty = timing.observe({"", false, -1.0f}, true, 1000, 1600);
    CHECK(empty.published && !empty.nonempty && empty.first_nonempty_ms == 400 && empty.nonempty_gap_ms < 0);
    const auto discarded = timing.observe({"未发布候选", false, -1.0f}, false, 1000, 1800);
    CHECK(!discarded.published && discarded.first_nonempty_ms == 400 && discarded.nonempty_gap_ms < 0);
    const auto next = timing.observe({"更正后的完整预览", false, -1.0f}, true, 1000, 2200);
    CHECK(next.first_nonempty_ms == 400 && next.nonempty_gap_ms == 800);
    const auto later = timing.observe({"继续说话", false, -1.0f}, true, 1000, 2500);
    CHECK(later.first_nonempty_ms == 400 && later.nonempty_gap_ms == 300);
}

HMI_TEST(preview_publication_timing_reset_and_missing_clock_do_not_invent_a_first_text_delay)
{
    hmi::app::PreviewPublicationTiming timing;
    CHECK(timing.observe({"未开始", false, -1.0f}, true, 0, 1000).first_nonempty_ms < 0);
    CHECK(timing.observe({"错误时刻", false, -1.0f}, true, 1000, 999).first_nonempty_ms < 0);
    const auto final = timing.observe({"终稿", true, -1.0f}, true, 1000, 1200);
    CHECK(!final.published && final.first_nonempty_ms < 0);
    CHECK(timing.observe({"首段", false, -1.0f}, true, 1000, 1400).first_nonempty_ms == 400);
    timing.reset();
    const auto restarted = timing.observe({"新一次录音", false, -1.0f}, true, 5000, 5500);
    CHECK(restarted.first_nonempty_ms == 500 && restarted.nonempty_gap_ms < 0);
    hmi::app::Recording recording;
    recording.started_at_ms = 5000;
    recording = {};
    CHECK(recording.started_at_ms == 0);
}

HMI_TEST(recording_release_stops_new_previews_before_the_device_finishes_closing)
{
    hmi::app::Recording recording;
    recording.started = true;
    hmi::app::CaptureGate gate;
    gate.phase = hmi::app::CaptureGate::Phase::Recording;
    CHECK(hmi::app::recording_allows_preview(recording, gate, true));
    // The microphone may still be closing; neither condition needs ended=true.
    CHECK(!hmi::app::recording_allows_preview(recording, gate, false));
    gate.stop_requested = true;
    CHECK(!hmi::app::recording_allows_preview(recording, gate, true));
    gate.stop_requested = false;
    recording.ended = true;
    CHECK(!hmi::app::recording_allows_preview(recording, gate, true));
}

HMI_TEST(input_delivery_aggregation_preserves_uncertainty_and_transport_semantics)
{
    using Delivery = hmi::action::InputDelivery;
    const auto merge = [](Delivery first, Delivery second) {
        hmi::action::InsertResult total;
        hmi::action::InsertResult item;
        item.ok = first != Delivery::NotSent;
        item.delivery = first;
        hmi::app::accumulate_input_result(total, item, 0);
        item.ok = second != Delivery::NotSent;
        item.delivery = second;
        hmi::app::accumulate_input_result(total, item, 1);
        return total;
    };
    for (const auto delivery : {Delivery::Verified, Delivery::DeliveredUnverified})
    {
        const auto mismatch_first = merge(Delivery::Mismatch, delivery);
        const auto mismatch_last = merge(delivery, Delivery::Mismatch);
        CHECK(mismatch_first.delivery == Delivery::Mismatch && mismatch_last.delivery == Delivery::Mismatch);
        CHECK(!mismatch_first.ok && !mismatch_last.ok);
        CHECK(merge(Delivery::Unknown, delivery).delivery == Delivery::Unknown);
        CHECK(merge(delivery, Delivery::Unknown).delivery == Delivery::Unknown);
    }
    CHECK(merge(Delivery::Verified, Delivery::Verified).delivery == Delivery::Verified);
    const auto unverified = merge(Delivery::Verified, Delivery::DeliveredUnverified);
    CHECK(unverified.delivery == Delivery::DeliveredUnverified && unverified.ok);
    CHECK(merge(Delivery::NotSent, Delivery::Verified).delivery == Delivery::Unknown);
    CHECK(merge(Delivery::Verified, Delivery::NotSent).delivery == Delivery::Unknown);
    CHECK(merge(Delivery::NotSent, Delivery::NotSent).delivery == Delivery::NotSent);
}

HMI_TEST(input_diagnostics_keep_unknown_values_null_and_report_font_without_a_cause_claim)
{
    hmi::action::InsertResult input;
    input.ok = true;
    input.delivery = hmi::action::InputDelivery::DeliveredUnverified;
    input.send_input_attempted = true;
    input.send_input_calls = 1;
    const auto unknown = hmi::app::input_result_fields(input, 1);
    CHECK(std::get<std::string>(unknown.find("delivery")->data) == "delivered_unverified");
    CHECK(!std::get<bool>(unknown.find("verification_attempted")->data));
    for (const auto *key : {"verification_before_ms", "verification_after_ms", "verification_reason", "control_class",
                            "control_unicode", "font_name", "font_evidence", "modifiers"})
        CHECK(std::holds_alternative<std::nullptr_t>(unknown.find(key)->data));
    input.delivery = hmi::action::InputDelivery::Verified;
    input.verification_attempted = true;
    input.verification_before_ms = 2.5;
    input.verification_after_ms = 3.5;
    input.verification_reason = "selection_edit_matched";
    input.control_class = "RichEditD2DPT";
    input.control_unicode = 1;
    input.modifiers = 2;
    input.modifiers_observed = true;
    input.font_name = "Microsoft YaHei";
    const auto known = hmi::app::input_result_fields(input, 1);
    CHECK(std::get<bool>(known.find("control_unicode")->data));
    CHECK(std::get<double>(known.find("modifiers")->data) == 2.0);
    CHECK(std::get<double>(known.find("verification_before_ms")->data) == 2.5);
    CHECK(std::get<double>(known.find("verification_after_ms")->data) == 3.5);
    CHECK(std::get<std::string>(known.find("font_name")->data) == input.font_name);
    CHECK(std::get<std::string>(known.find("font_evidence")->data) == "provider_report_only");
    CHECK(!known.find("font_problem") && !known.find("font_verified"));
}

HMI_TEST(input_aggregation_keeps_mismatch_evidence_and_does_not_invent_complete_verification_timing)
{
    hmi::action::InsertResult first;
    first.ok = true; // Defensive host normalization must still reject an explicit mismatch.
    first.delivery = hmi::action::InputDelivery::Mismatch;
    first.verification_attempted = true;
    first.verification_before_ms = 2.0;
    first.verification_after_ms = -1.0;
    first.verification_reason = "observed_text_differs";
    first.control_class = "Edit";
    first.control_unicode = 1;
    first.font_name = "First font";
    first.modifiers = 1;
    first.modifiers_observed = true;
    first.send_input_calls = 1;
    hmi::action::InsertResult total;
    hmi::app::accumulate_input_result(total, first, 0);
    auto later = first;
    later.delivery = hmi::action::InputDelivery::Verified;
    later.verification_before_ms = 3.0;
    later.verification_after_ms = 4.0;
    later.verification_reason = "selection_edit_matched";
    later.control_class = "Other editor";
    later.control_unicode = 0;
    later.font_name = "Other font";
    later.modifiers = 2;
    hmi::app::accumulate_input_result(total, later, 1);
    CHECK(!total.ok && total.delivery == hmi::action::InputDelivery::Mismatch);
    CHECK(total.verification_reason == "observed_text_differs");
    CHECK(total.verification_before_ms == 5.0 && total.verification_after_ms < 0.0);
    CHECK(total.control_class.empty() && total.font_name.empty() && total.control_unicode < 0);
    CHECK(total.modifiers == 3 && total.modifiers_observed && total.send_input_calls == 2);
    const auto fields = hmi::app::input_result_fields(total, 2);
    CHECK(std::holds_alternative<std::nullptr_t>(fields.find("verification_after_ms")->data));
    CHECK(std::holds_alternative<std::nullptr_t>(fields.find("font_name")->data));
}

HMI_TEST(dictation_unmatched_transcript_uses_one_caret_text_action)
{
    auto rules = hmi::intent::default_rules();
    hmi::app::Pending pending;
    pending.transcript.text = "  请帮我检查 Codex 的输入框 ＡＢＣ。  ";
    pending.transcript.confidence = -1.0f;
    pending.released_at = 1000;
    pending.target.pid = 42;
    pending.target.native_window = 100;
    pending.target.process = "Codex.exe";
    pending.window = reinterpret_cast<HWND>(pending.target.native_window);
    const auto job = hmi::app::transcript_job(rules, pending, 2000);
    CHECK(job.event.rule == "Dictation");
    CHECK(job.event.transcript == pending.transcript.text);
    CHECK(job.event.confidence == -1.0f);
    CHECK(job.event.at_ms == pending.released_at);
    CHECK(job.target.pid == pending.target.pid);
    CHECK(job.target.native_window == pending.target.native_window);
    CHECK(job.window == pending.window);
    CHECK(job.actions.size() == 1);
    if (job.actions.size() != 1) return;
    CHECK(job.actions[0].kind == "text");
    CHECK(job.actions[0].params.at("target") == "sendinput");
    CHECK(job.actions[0].params.at("text") == pending.transcript.text);

    unsigned insertions = 0;
    std::string inserted;
    hmi::action::Context context;
    context.insert_text = [&](std::string_view text) {
        ++insertions;
        inserted = text;
        return true;
    };
    const auto result = hmi::action::execute(job.actions[0], context);
    CHECK(result.ok);
    CHECK(insertions == 1);
    CHECK(inserted == pending.transcript.text);
}

HMI_TEST(dictation_keeps_matching_command_instead_of_typing_it)
{
    auto rules = hmi::intent::default_rules();
    hmi::app::Pending pending;
    pending.transcript.text = "打开计算器。";
    pending.transcript.confidence = -1.0f;
    const auto job = hmi::app::transcript_job(rules, pending, 2000);
    CHECK(job.event.rule == "启动计算器");
    CHECK(job.actions.size() == 1);
    if (job.actions.size() != 1) return;
    CHECK(job.actions[0].kind == "launch");
    CHECK(job.actions[0].params.at("path") == "calc.exe");
    CHECK(rules.rules.front().last_fired_ms == 2000);
}

HMI_TEST(dictation_ignores_blank_results_and_does_not_replace_empty_command_actions)
{
    auto rules = hmi::intent::default_rules();
    hmi::app::Pending pending;
    for (const std::string &text : {std::string{}, std::string(" \t\r\n　")})
    {
        pending.transcript.text = text;
        const auto job = hmi::app::transcript_job(rules, pending, 2000);
        CHECK(job.actions.empty());
        CHECK(job.event.rule.empty());
    }
    pending.transcript.text = "打开计算器";
    pending.transcript.confidence = -1.0f;
    rules.rules.front().actions.clear();
    const auto job = hmi::app::transcript_job(rules, pending, 2000);
    CHECK(job.event.rule == "启动计算器");
    CHECK(job.actions.empty());
}

HMI_TEST(dictation_does_not_type_commands_suppressed_by_cooldown_or_confidence)
{
    auto rules = hmi::intent::default_rules();
    hmi::app::Pending pending;
    pending.transcript.text = "打开计算器。";
    pending.transcript.confidence = -1.0f;
    const auto first = hmi::app::transcript_job(rules, pending, 2000);
    CHECK(first.actions.size() == 1);
    const auto cooldown = hmi::app::transcript_job(rules, pending, 2100);
    CHECK(cooldown.actions.empty());
    CHECK(cooldown.event.rule == "启动计算器");
    CHECK(cooldown.event.detail.find("Command suppressed") != std::string::npos);
    CHECK(rules.rules.front().last_fired_ms == 2000);

    rules.rules.front().last_fired_ms = 0;
    rules.rules.front().trigger.min_confidence = 0.8f;
    pending.transcript.confidence = 0.2f;
    const auto confidence = hmi::app::transcript_job(rules, pending, 3000);
    CHECK(confidence.actions.empty());
    CHECK(confidence.event.rule == "启动计算器");
    CHECK(confidence.event.detail.find("Command suppressed") != std::string::npos);
    CHECK(rules.rules.front().last_fired_ms == 0);

    rules.rules.front().enabled = false;
    const auto disabled = hmi::app::transcript_job(rules, pending, 3000);
    CHECK(disabled.event.rule == "Dictation");
    CHECK(disabled.actions.size() == 1);
}

HMI_TEST(capture_gate_only_opens_on_an_available_shortcut_and_closes_on_release)
{
    using Gate = hmi::app::CaptureGate;
    using Event = hmi::input::HotkeyEvent;
    Gate gate;
    CHECK(gate.phase == Gate::Phase::Idle);
    CHECK(gate.update(Event::HoldBegin) == Gate::Request::Rejected);
    CHECK(gate.phase == Gate::Phase::Idle);
    CHECK(gate.update(Event::HoldEnd) == Gate::Request::None);
    gate.available = true;
    CHECK(gate.update(Event::HoldBegin) == Gate::Request::Start);
    CHECK(gate.phase == Gate::Phase::Opening);
    CHECK(gate.update(Event::HoldBegin) == Gate::Request::None);
    gate.phase = Gate::Phase::Recording;
    CHECK(gate.update(Event::HoldEnd) == Gate::Request::Stop);
    CHECK(gate.stop_requested);
    gate.finish(true);
    CHECK(gate.phase == Gate::Phase::Processing);
    CHECK(!gate.stop_requested);
}

HMI_TEST(capture_gate_rejects_new_audio_during_inference_and_does_not_replay_a_held_key)
{
    using Gate = hmi::app::CaptureGate;
    using Event = hmi::input::HotkeyEvent;
    Gate gate;
    gate.available = true;
    gate.finish(true);
    CHECK(gate.update(Event::Toggle) == Gate::Request::Rejected);
    CHECK(!gate.toggled && !gate.requested);
    CHECK(gate.update(Event::HoldBegin) == Gate::Request::Rejected);
    CHECK(gate.phase == Gate::Phase::Processing);
    gate.finish();
    CHECK(gate.phase == Gate::Phase::Idle);
    CHECK(gate.update(Event::HoldBegin) == Gate::Request::None);
    CHECK(gate.phase == Gate::Phase::Idle);
    CHECK(gate.update(Event::HoldEnd) == Gate::Request::None);
    CHECK(gate.update(Event::HoldBegin) == Gate::Request::Start);
    CHECK(gate.update(Event::HoldEnd) == Gate::Request::Stop);
    CHECK(gate.phase == Gate::Phase::Opening && gate.stop_requested);
    // The controller can discard a press released before the device opened.
    gate.finish();
    CHECK(gate.phase == Gate::Phase::Idle && !gate.stop_requested);
    gate.available = false;
    CHECK(gate.update(Event::Toggle) == Gate::Request::Rejected);
    CHECK(gate.phase == Gate::Phase::Idle && !gate.toggled);
}

HMI_TEST(capture_ring_wraps_without_reallocation_or_overwriting_unread_audio)
{
    hmi::app::AudioRing ring;
    ring.configure(8);
    const auto *allocation = ring.storage.data();
    const float first[]{0, 1, 2, 3, 4, 5};
    const float second[]{6, 7, 8, 9, 10, 11};
    float output[8]{};
    CHECK(ring.push(first, 6));
    CHECK(ring.pop(output, 4) == 4);
    for (std::size_t index = 0; index < 4; ++index) CHECK(output[index] == static_cast<float>(index));
    CHECK(ring.push(second, 6));
    CHECK(ring.queued == 8 && ring.high_water == 8);
    const float extra = 12;
    CHECK(!ring.push(&extra, 1));
    CHECK(ring.queued == 8 && ring.storage.data() == allocation);
    CHECK(ring.pop(output, 8) == 8);
    for (std::size_t index = 0; index < 8; ++index) CHECK(output[index] == static_cast<float>(index + 4));
    CHECK(ring.queued == 0 && ring.high_water == 8);
    ring.clear();
    CHECK(ring.high_water == 0 && ring.storage.data() == allocation);
}

HMI_TEST(capture_ring_only_releases_a_short_tail_after_microphone_closes)
{
    hmi::app::AudioRing ring;
    ring.configure(10);
    const float samples[]{10, 11, 12, 13, 14, 15, 16};
    float chunk[4]{};
    CHECK(ring.push(samples, 7));
    CHECK(ring.next_chunk(4, false) == 4);
    CHECK(ring.pop(chunk, ring.next_chunk(4, false)) == 4);
    CHECK(chunk[0] == 10 && chunk[3] == 13);
    CHECK(ring.next_chunk(4, false) == 0);
    CHECK(ring.next_chunk(4, true) == 3);
    CHECK(ring.pop(chunk, ring.next_chunk(4, true)) == 3);
    CHECK(chunk[0] == 14 && chunk[2] == 16 && ring.queued == 0);
    CHECK(ring.next_chunk(4, true) == 0);
}

HMI_TEST(capture_statistics_use_real_rms_and_count_silent_samples)
{
    hmi::app::Recording recording;
    recording.started = true;
    recording.request_id = 41;
    hmi::app::CaptureGate gate;
    gate.available = true;
    gate.phase = hmi::app::CaptureGate::Phase::Recording;
    hmi::app::AudioRing ring;
    ring.configure(2880);
    std::array<float, 1440> samples{};
    for (std::size_t index = 0; index < samples.size(); index += 2) samples[index] = 0.5f;
    CHECK(hmi::app::append_recording_audio(recording, ring, samples.data(), samples.size(), 16000) ==
        hmi::app::AudioFeed::Accepted);
    hmi::app::VoiceSnapshot out;
    hmi::app::copy_capture_statistics(out, recording, gate, true, false);
    CHECK(out.capture_active && out.capture_request_id == 41 && out.captured_frames == 1440);
    CHECK(std::abs(out.capture_levels.back() - std::sqrt(0.125f)) < 0.000001f);
    CHECK(std::all_of(out.capture_levels.begin(), out.capture_levels.end() - 1, [](float value) { return value == 0.0f; }));
    samples.fill(0.0f);
    CHECK(hmi::app::append_recording_audio(recording, ring, samples.data(), samples.size(), 16000) ==
        hmi::app::AudioFeed::Accepted);
    hmi::app::copy_capture_statistics(out, recording, gate, true, false);
    CHECK(out.captured_frames == 2880 && out.capture_active && out.capture_live_level == 0.0f);
    CHECK(std::abs(out.capture_levels[62] - std::sqrt(0.125f)) < 0.000001f && out.capture_levels.back() == 0.0f);
}

HMI_TEST(capture_statistics_keep_ninety_millisecond_buckets_across_callbacks)
{
    hmi::app::Recording recording;
    hmi::app::AudioRing ring;
    ring.configure(2880);
    std::array<float, 960> first{};
    first.fill(0.5f);
    const std::array<float, 960> silence{};
    CHECK(hmi::app::append_recording_audio(recording, ring, first.data(), first.size(), 16000) == hmi::app::AudioFeed::Accepted);
    CHECK(hmi::app::append_recording_audio(recording, ring, silence.data(), silence.size(), 16000) == hmi::app::AudioFeed::Accepted);
    hmi::app::VoiceSnapshot out;
    hmi::app::CaptureGate gate;
    hmi::app::copy_capture_statistics(out, recording, gate, false, false);
    CHECK(out.captured_frames == 1920 && std::abs(out.capture_levels.back() - std::sqrt(1.0f / 6.0f)) < 0.000001f);
    std::array<float, 480> tail{};
    tail.fill(1.0f);
    CHECK(hmi::app::append_recording_audio(recording, ring, tail.data(), tail.size(), 16000) == hmi::app::AudioFeed::Accepted);
    hmi::app::copy_capture_statistics(out, recording, gate, false, false);
    CHECK(std::abs(out.capture_live_level - std::sqrt(0.5f)) < 0.000001f);
    tail.fill(0.0f);
    CHECK(hmi::app::append_recording_audio(recording, ring, tail.data(), tail.size(), 16000) == hmi::app::AudioFeed::Accepted);
    hmi::app::copy_capture_statistics(out, recording, gate, false, false);
    CHECK(out.captured_frames == 2880 && out.capture_live_level == 0.0f);
    CHECK(std::abs(out.capture_levels[62] - std::sqrt(1.0f / 6.0f)) < 0.000001f);
    CHECK(std::abs(out.capture_levels.back() - std::sqrt(1.0f / 3.0f)) < 0.000001f);
}

HMI_TEST(capture_statistics_wrap_fixed_history_in_chronological_order)
{
    hmi::app::Recording recording;
    hmi::app::AudioRing ring;
    ring.configure(70 * 1440);
    std::array<float, 1440> samples{};
    for (std::size_t bucket = 1; bucket <= 70; ++bucket)
    {
        samples.fill(static_cast<float>(bucket) / 128.0f);
        CHECK(hmi::app::append_recording_audio(recording, ring, samples.data(), samples.size(), 160000) == hmi::app::AudioFeed::Accepted);
    }
    hmi::app::VoiceSnapshot out;
    hmi::app::copy_capture_statistics(out, recording, {}, false, false);
    CHECK(out.captured_frames == 70 * 1440 && out.capture_live_level == 0.0f);
    for (std::size_t index = 0; index < out.capture_levels.size(); ++index)
        CHECK(std::abs(out.capture_levels[index] - static_cast<float>(index + 7) / 128.0f) < 0.000001f);
}

HMI_TEST(capture_statistics_ignore_rejected_limit_tail_without_changing_asr_pcm_or_peak)
{
    hmi::app::Recording recording;
    hmi::app::AudioRing ring;
    ring.configure(1444);
    std::array<float, 1440> first{};
    first.fill(0.25f);
    const std::array<float, 6> tail{0.5f, -0.5f, 0.75f, -0.75f, 12.0f, 14.0f};
    CHECK(hmi::app::append_recording_audio(recording, ring, first.data(), first.size(), 1444) == hmi::app::AudioFeed::Accepted);
    CHECK(hmi::app::append_recording_audio(recording, ring, tail.data(), tail.size(), 1444) == hmi::app::AudioFeed::LimitReached);
    CHECK(hmi::app::append_recording_audio(recording, ring, tail.data(), tail.size(), 1444) == hmi::app::AudioFeed::LimitReached);
    hmi::app::VoiceSnapshot out;
    hmi::app::copy_capture_statistics(out, recording, {}, true, false);
    CHECK(out.captured_frames == 1444 && !out.capture_active && recording.peak == 0.75f);
    CHECK(out.capture_levels.back() == 0.25f && std::abs(out.capture_live_level - std::sqrt(0.40625f)) < 0.000001f);
    std::array<float, 1444> captured{};
    CHECK(ring.pop(captured.data(), captured.size()) == captured.size());
    CHECK(std::equal(first.begin(), first.end(), captured.begin()));
    CHECK(std::equal(tail.begin(), tail.begin() + 4, captured.begin() + 1440));
}

HMI_TEST(capture_statistics_reset_between_recording_requests)
{
    hmi::app::Recording recording;
    recording.started = true;
    recording.request_id = 71;
    hmi::app::AudioRing ring;
    ring.configure(1500);
    std::array<float, 1500> samples{};
    samples.fill(0.5f);
    CHECK(hmi::app::append_recording_audio(recording, ring, samples.data(), samples.size(), 16000) == hmi::app::AudioFeed::Accepted);
    hmi::app::VoiceSnapshot out;
    hmi::app::copy_capture_statistics(out, recording, {}, false, false);
    CHECK(out.capture_levels.back() == 0.5f && out.capture_live_level == 0.5f);
    recording = {};
    recording.request_id = 72;
    hmi::app::copy_capture_statistics(out, recording, {}, true, false);
    CHECK(out.capture_request_id == 72 && out.captured_frames == 0 && !out.capture_active && out.capture_live_level == 0.0f);
    CHECK(std::all_of(out.capture_levels.begin(), out.capture_levels.end(), [](float value) { return value == 0.0f; }));
    CHECK(recording.peak == 0.0f && recording.capture_level_write == 0 && recording.capture_level_count == 0);
    CHECK(recording.capture_sum_squares == 0.0 && recording.capture_bucket_samples == 0);
}

HMI_TEST(capture_statistics_stop_animation_at_each_capture_boundary)
{
    using Gate = hmi::app::CaptureGate;
    using Recording = hmi::app::Recording;
    Recording recording;
    recording.started = true;
    Gate gate;
    gate.available = true;
    gate.phase = Gate::Phase::Recording;
    const auto active = [](const Recording &item, const Gate &capture_gate, bool accepting = true, bool stopping = false) {
        hmi::app::VoiceSnapshot out;
        hmi::app::copy_capture_statistics(out, item, capture_gate, accepting, stopping);
        return out.capture_active;
    };
    CHECK(active(recording, gate) && !active(recording, gate, false) && !active(recording, gate, true, true));
    for (const auto phase : {Gate::Phase::Idle, Gate::Phase::Opening, Gate::Phase::Processing})
    {
        auto other = gate;
        other.phase = phase;
        CHECK(!active(recording, other));
    }
    gate.stop_requested = true;
    CHECK(!active(recording, gate));
    gate.stop_requested = false;
    auto other = recording;
    other.started = false;
    CHECK(!active(other, gate));
    other = recording;
    other.ended = true;
    CHECK(!active(other, gate));
    other = recording;
    other.limit_reached = true;
    CHECK(!active(other, gate));
    for (const auto discard : {Recording::Discard::Overrun, Recording::Discard::Inference, Recording::Discard::Shortcut})
    {
        other = recording;
        other.discard = discard;
        CHECK(!active(other, gate));
    }
}

namespace
{
struct WarmupSession
{
    bool begin_ok = true;
    bool push_ok = true;
    bool finish_ok = true;
    bool silence_only = false;
    bool preview_allowed = true;
    unsigned begins = 0;
    unsigned pushes = 0;
    unsigned finishes = 0;
    unsigned resets = 0;
    bool begin(std::string *) { ++begins; return begin_ok; }
    bool push(const float *samples, std::size_t frames, std::vector<hmi::engine::Transcript> &events, std::string *, bool allow_preview)
    {
        ++pushes;
        preview_allowed = allow_preview;
        silence_only = frames == 16000 && std::all_of(samples, samples + frames, [](float value) { return value == 0.0f; });
        events.push_back({"warmup output must stay private", true, -1.0f});
        return push_ok;
    }
    bool finish(hmi::engine::Transcript &out, std::string *)
    {
        ++finishes;
        out.text = "warmup output must not enter the action queue";
        return finish_ok;
    }
    void reset() { ++resets; }
};
}

HMI_TEST(asr_preparation_uses_sdk_silence_without_capture_and_resets_before_ready)
{
    WarmupSession session;
    CHECK(hmi::app::prepare_recognition(session, nullptr));
    CHECK(session.begins == 1 && session.pushes == 1 && session.finishes == 1);
    CHECK(session.silence_only && !session.preview_allowed && session.resets == 1);
    WarmupSession failed_push;
    failed_push.push_ok = false;
    CHECK(!hmi::app::prepare_recognition(failed_push, nullptr));
    CHECK(failed_push.pushes == 1 && failed_push.finishes == 0 && failed_push.resets == 1);
    WarmupSession failed_finish;
    failed_finish.finish_ok = false;
    CHECK(!hmi::app::prepare_recognition(failed_finish, nullptr));
    CHECK(failed_finish.finishes == 1 && failed_finish.resets == 1);
}

HMI_TEST(asr_drafts_replace_the_whole_preview_and_never_dispatch_external_actions)
{
    hmi::app::VoiceSnapshot view;
    hmi::app::apply_preview(view, {"打开计算", false, -1.0f});
    hmi::app::apply_preview(view, {"打开计算器。", false, -1.0f});
    CHECK(view.last_transcript == "打开计算器。" && view.partial_revision == 2 && !view.transcript_final);
    auto rules = hmi::intent::default_rules();
    hmi::app::Pending pending;
    pending.transcript = {view.last_transcript, false, -1.0f};
    const auto draft = hmi::app::transcript_job(rules, pending, 2000);
    CHECK(draft.actions.empty());
    CHECK(draft.event.detail == "Draft transcription is display-only");
    CHECK(rules.rules.front().last_fired_ms == 0);
    hmi::app::apply_preview(view, {"", false, -1.0f});
    CHECK(view.last_transcript.empty() && view.partial_revision == 3);
    hmi::app::apply_preview(view, {"ignored final event", true, -1.0f});
    CHECK(view.last_transcript.empty() && view.partial_revision == 3);
    pending.transcript.final = true;
    const auto final = hmi::app::transcript_job(rules, pending, 2000);
    CHECK(final.actions.size() == 1 && final.actions.front().kind == "launch");
    CHECK(rules.rules.front().last_fired_ms == 2000);
}

HMI_TEST(capture_limit_clips_only_the_excess_audio_and_keeps_the_full_segment)
{
    hmi::app::Recording recording;
    hmi::app::AudioRing ring;
    ring.configure(8);
    const auto *allocation = ring.storage.data();
    const float first[]{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    const float tail[]{0.7f, 0.8f, 12.0f, 14.0f};
    CHECK(hmi::app::append_recording_audio(recording, ring, first, 6, 8) == hmi::app::AudioFeed::Accepted);
    CHECK(hmi::app::append_recording_audio(recording, ring, tail, 4, 8) == hmi::app::AudioFeed::LimitReached);
    CHECK(recording.limit_reached && recording.frames == 8 && recording.peak == 0.8f);
    CHECK(recording.discard == hmi::app::Recording::Discard::None);
    CHECK(ring.queued == 8 && ring.storage.data() == allocation);
    CHECK(hmi::app::append_recording_audio(recording, ring, tail, 4, 8) == hmi::app::AudioFeed::LimitReached);
    CHECK(recording.frames == 8 && ring.queued == 8);
    float captured[8]{};
    CHECK(ring.pop(captured, 8) == 8);
    for (int index = 0; index < 6; ++index) CHECK(captured[index] == first[index]);
    CHECK(captured[6] == tail[0] && captured[7] == tail[1]);
}

HMI_TEST(capture_limit_stops_at_the_exact_boundary_but_overrun_remains_an_error)
{
    hmi::app::Recording recording;
    hmi::app::AudioRing ring;
    ring.configure(4);
    const float samples[]{1, 2, 3, 4};
    CHECK(hmi::app::append_recording_audio(recording, ring, samples, 4, 4) == hmi::app::AudioFeed::LimitReached);
    CHECK(recording.frames == 4 && recording.discard == hmi::app::Recording::Discard::None);
    recording = {};
    ring.configure(3);
    CHECK(hmi::app::append_recording_audio(recording, ring, samples, 4, 8) == hmi::app::AudioFeed::Failed);
    CHECK(recording.discard == hmi::app::Recording::Discard::Overrun && recording.frames == 0 && ring.queued == 0);
}

HMI_TEST(capture_limit_finalizes_retained_audio_without_dispatching_before_release)
{
    struct Session
    {
        unsigned finishes = 0;
        bool finish(hmi::engine::Transcript &out, std::string *)
        {
            ++finishes;
            out = {"完整的三十秒终稿", true, -1.0f};
            return true;
        }
    } session;
    hmi::app::Recording recording;
    recording.started = recording.ended = recording.limit_reached = true;
    recording.frames = 30 * 16000;
    hmi::engine::Transcript final;
    CHECK(hmi::app::finish_recording(session, recording, final, nullptr));
    CHECK(session.finishes == 1 && final.final && final.text == "完整的三十秒终稿");
    hmi::app::CaptureGate gate;
    gate.available = gate.held = gate.requested = true;
    gate.finish(true);
    CHECK(hmi::app::release_pending(recording, gate, false));
    int resolutions = 0;
    const auto resolve = [&] {
        ++resolutions;
        hmi::action::Target target;
        target.pid = 42;
        target.native_window = 100;
        return target;
    };
    CHECK(!hmi::app::bind_recording_release(recording, gate, hmi::app::CaptureGate::Request::None, 31000, resolve));
    CHECK(resolutions == 0 && recording.target.pid == 0);
    const auto release = gate.update(hmi::input::HotkeyEvent::HoldEnd);
    CHECK(release == hmi::app::CaptureGate::Request::None);
    CHECK(hmi::app::bind_recording_release(recording, gate, release, 36000, resolve));
    CHECK(!hmi::app::release_pending(recording, gate, false));
    CHECK(recording.released_at == 36000 && recording.target.pid == 42);
    CHECK(recording.target.native_window == 100);
    CHECK(recording.window == reinterpret_cast<HWND>(recording.target.native_window));
    CHECK(!hmi::app::bind_recording_release(recording, gate, release, 37000, resolve));
    CHECK(resolutions == 1 && recording.released_at == 36000 && session.finishes == 1);
    recording.discard = hmi::app::Recording::Discard::Overrun;
    CHECK(!hmi::app::finish_recording(session, recording, final, nullptr));
    CHECK(session.finishes == 1);
}

HMI_TEST(capture_limit_toggle_release_and_shutdown_or_shortcut_failure_end_the_wait)
{
    hmi::app::Recording recording;
    recording.started = recording.limit_reached = true;
    hmi::app::CaptureGate gate;
    gate.available = gate.toggled = gate.requested = true;
    gate.finish(true);
    CHECK(hmi::app::release_pending(recording, gate, false));
    CHECK(!hmi::app::release_pending(recording, gate, true));
    gate.available = false;
    CHECK(!hmi::app::release_pending(recording, gate, false));
    gate.available = true;
    recording.discard = hmi::app::Recording::Discard::Shortcut;
    CHECK(!hmi::app::release_pending(recording, gate, false));
    recording.discard = hmi::app::Recording::Discard::None;
    const auto release = gate.update(hmi::input::HotkeyEvent::Toggle);
    CHECK(hmi::app::bind_recording_release(recording, gate, release, 35000, [] { return hmi::action::Target{}; }));
    CHECK(!hmi::app::release_pending(recording, gate, false));
}

HMI_TEST(speech_preview_queue_validates_text_and_stays_on_the_speech_route)
{
    std::deque<hmi::app::ActionJob> jobs;
    std::string error;
    CHECK(hmi::app::enqueue_speech(jobs, "你好，这是试听。", 1000, &error));
    CHECK(error.empty() && jobs.size() == 1);
    if (jobs.empty()) return;
    CHECK(jobs.front().event.at_ms == 1000 && jobs.front().actions.size() == 1);
    CHECK(jobs.front().event.asr_ms < 0.0 && jobs.front().event.tts_ms < 0.0);
    CHECK(jobs.front().actions.front().kind == "speak");
    int spoken = 0;
    int inserted = 0;
    hmi::action::Context context;
    context.speak = [&](std::string_view text) { ++spoken; return text == "你好，这是试听。"; };
    context.insert_text = [&](std::string_view) { ++inserted; return true; };
    CHECK(hmi::action::execute(jobs.front().actions.front(), context).ok);
    CHECK(spoken == 1 && inserted == 0);
    for (const auto &text : {std::string{}, std::string(" \r\n\t　"), std::string("bad\0text", 8),
                            std::string("\xff", 1), std::string(4097, 'a')})
        CHECK(!hmi::app::enqueue_speech(jobs, text, 2000, &error));
    CHECK(jobs.size() == 1);
    for (int index = 0; index < 15; ++index)
        CHECK(hmi::app::enqueue_speech(jobs, "后续试听", 3000, &error));
    CHECK(!hmi::app::enqueue_speech(jobs, "队列已满", 4000, &error));
    CHECK(jobs.size() == 16 && !error.empty());
}

HMI_TEST(tts_switch_removes_only_pending_speech_and_is_closed_by_default)
{
    std::deque<hmi::app::ActionJob> jobs;
    std::string error;
    CHECK(hmi::app::enqueue_speech(jobs, "待播放内容", 1000, &error));
    hmi::app::ActionJob mixed;
    mixed.actions.push_back({"speak", {{"text", "这一项应取消"}}});
    mixed.actions.push_back({"key", {{"chord", "Return"}}});
    jobs.push_back(std::move(mixed));
    hmi::app::remove_pending_speech(jobs);
    CHECK(jobs.size() == 1 && jobs.front().actions.size() == 1);
    CHECK(jobs.front().actions.front().kind == "key");
    hmi::app::DictationDecisionRuntime runtime;
    CHECK(!runtime.snapshot().tts_enabled);
    runtime.set_tts_enabled(true);
    CHECK(runtime.snapshot().tts_enabled);
    runtime.set_tts_enabled(false);
    CHECK(!runtime.snapshot().tts_enabled && !runtime.snapshot().tts_busy);
    CHECK(!runtime.request_speech("关闭时不能朗读", &error));
}

HMI_TEST(removing_pending_speech_collects_only_removed_jobs_with_request_identity)
{
    std::deque<hmi::app::ActionJob> jobs;
    std::string error;
    CHECK(hmi::app::enqueue_speech(jobs, "第一条待播放", 1000, &error));
    jobs.back().event.request_id = 701;
    hmi::app::ActionJob mixed;
    mixed.event.request_id = 702;
    mixed.event.transcript = "保留混合任务的文字";
    mixed.actions.push_back({"speak", {{"text", "取消混合任务的朗读"}}});
    mixed.actions.push_back({"text", {{"text", "保留混合任务的文字"}}});
    jobs.push_back(std::move(mixed));
    CHECK(hmi::app::enqueue_speech(jobs, "第二条待播放", 2000, &error));
    jobs.back().event.request_id = 703;
    std::vector<hmi::app::VoiceEvent> removed;
    hmi::app::remove_pending_speech(jobs, &removed);
    CHECK(removed.size() == 2 && jobs.size() == 1);
    if (removed.size() != 2 || jobs.size() != 1) return;
    CHECK(removed[0].request_id == 701 && removed[0].transcript == "第一条待播放");
    CHECK(removed[1].request_id == 703 && removed[1].transcript == "第二条待播放");
    CHECK(jobs.front().event.request_id == 702);
    CHECK(jobs.front().event.transcript == "保留混合任务的文字");
    CHECK(jobs.front().actions.size() == 1 && jobs.front().actions.front().kind == "text");
    CHECK(jobs.front().actions.front().params.at("text") == "保留混合任务的文字");
    hmi::app::remove_pending_speech(jobs, &removed);
    CHECK(removed.size() == 2 && jobs.size() == 1);
}

HMI_TEST(speech_requests_are_blocked_during_capture_or_finalization_and_after_stop)
{
    hmi::app::VoiceSnapshot view;
    CHECK(!hmi::app::speech_conflicts_with_capture(view));
    view.state = hmi::app::VoiceState::Starting;
    CHECK(hmi::app::speech_conflicts_with_capture(view));
    view.state = hmi::app::VoiceState::Listening;
    CHECK(hmi::app::speech_conflicts_with_capture(view));
    view.state = hmi::app::VoiceState::Stopped;
    view.refining = true;
    CHECK(hmi::app::speech_conflicts_with_capture(view));
    view.refining = false;
    view.awaiting_release = true;
    CHECK(hmi::app::speech_conflicts_with_capture(view));
    hmi::app::DictationDecisionRuntime runtime;
    std::string error;
    CHECK(!runtime.request_speech("测试", &error));
    CHECK(!error.empty() && !runtime.snapshot().tts_error.empty());
    runtime.stop();
    CHECK(!runtime.request_speech("测试", &error));
    CHECK(!runtime.snapshot().tts_available && !runtime.snapshot().tts_busy);
}

HMI_TEST(spoken_read_commands_route_to_tts_without_dictating_the_command)
{
    for (const auto &text : {"朗读", "请朗读。"})
    {
        auto rules = hmi::intent::default_rules();
        hmi::app::Pending pending;
        pending.transcript = {text, true, -1.0f};
        const auto job = hmi::app::transcript_job(rules, pending, 10000);
        CHECK(job.event.rule == "朗读");
        CHECK(job.actions.size() == 1 && job.actions.front().kind == "speak");
    }
}

HMI_TEST(final_dictation_queues_exact_text_input_then_independent_tts)
{
    auto rules = hmi::intent::default_rules();
    hmi::app::Pending pending;
    pending.transcript = {"你好，当前转写需要接入语音合成。", true, -1.0f};
    pending.released_at = 1000;
    pending.target.pid = 42;
    pending.asr_ms = 347.25;
    pending.request_id = 701;
    const auto job = hmi::app::transcript_job(rules, pending, 2000);
    CHECK(job.fallback_dictation);
    std::deque<hmi::app::ActionJob> jobs;
    CHECK(hmi::app::enqueue_transcript_actions(jobs, job, true, 7));
    CHECK(jobs.size() == 2);
    if (jobs.size() != 2) return;
    CHECK(jobs.front().actions.size() == 1 && jobs.front().actions.front().kind == "text");
    CHECK(!jobs.front().automatic_readback && jobs.front().speech_generation == 7);
    CHECK(jobs.back().automatic_readback && jobs.back().speech_generation == 7);
    CHECK(jobs.back().event.rule == "转写朗读");
    CHECK(jobs.back().event.at_ms == 1000);
    CHECK(jobs.front().event.request_id == 701 && jobs.back().event.request_id == 701);
    CHECK(jobs.front().queued_at_ms == 2000 && jobs.back().queued_at_ms == 2000);
    CHECK(jobs.front().event.asr_ms == pending.asr_ms && jobs.back().event.asr_ms == pending.asr_ms);
    CHECK(jobs.front().event.confidence == pending.transcript.confidence &&
        jobs.back().event.confidence == pending.transcript.confidence);
    CHECK(jobs.front().event.tts_ms < 0.0 && jobs.back().event.tts_ms < 0.0);
    std::vector<std::string> operations;
    hmi::action::Context context;
    context.insert_text = [&](std::string_view text) {
        operations.push_back("text");
        CHECK(text == pending.transcript.text);
        return true;
    };
    context.speak = [&](std::string_view text) {
        operations.push_back("speak");
        CHECK(text == pending.transcript.text);
        return true;
    };
    for (const auto &queued : jobs)
        for (const auto &action : queued.actions)
            CHECK(hmi::action::execute(action, context).ok);
    CHECK(operations == std::vector<std::string>({"text", "speak"}));
    hmi::app::remove_pending_speech(jobs);
    CHECK(jobs.size() == 1 && jobs.front().actions.front().kind == "text");
}

HMI_TEST(automatic_readback_requires_enabled_final_dictation_and_preserves_commands)
{
    auto rules = hmi::intent::default_rules();
    hmi::app::Pending pending;
    pending.transcript = {"普通语句最终稿。", true, -1.0f};
    std::deque<hmi::app::ActionJob> jobs;
    CHECK(!hmi::app::enqueue_transcript_actions(jobs,
        hmi::app::transcript_job(rules, pending, 1000), false, 1));
    CHECK(jobs.size() == 1 && jobs.front().actions.front().kind == "text");
    jobs.clear();
    for (const auto &text : {"", "　\t", "普通草稿"})
    {
        pending.transcript = {text, false, -1.0f};
        CHECK(!hmi::app::enqueue_transcript_actions(jobs,
            hmi::app::transcript_job(rules, pending, 1000), true, 2));
        CHECK(jobs.empty());
    }
    pending.transcript = {"朗读", true, -1.0f};
    CHECK(!hmi::app::enqueue_transcript_actions(jobs,
        hmi::app::transcript_job(rules, pending, 1000), true, 3));
    CHECK(jobs.size() == 1 && jobs.front().actions.size() == 1);
    CHECK(jobs.front().actions.front().kind == "speak" && !jobs.front().automatic_readback);
    jobs.clear();
    rules = hmi::intent::default_rules();
    rules.rules.front().name = "Dictation";
    pending.transcript = {"打开计算器", true, -1.0f};
    CHECK(!hmi::app::enqueue_transcript_actions(jobs,
        hmi::app::transcript_job(rules, pending, 1000), true, 4));
    CHECK(jobs.size() == 1 && jobs.front().actions.front().kind == "launch");
}

HMI_TEST(automatic_readback_keeps_text_when_only_one_queue_slot_remains)
{
    auto rules = hmi::intent::default_rules();
    hmi::app::Pending pending;
    pending.transcript = {"优先保留文字输入。", true, -1.0f};
    std::deque<hmi::app::ActionJob> jobs(15);
    CHECK(!hmi::app::enqueue_transcript_actions(jobs,
        hmi::app::transcript_job(rules, pending, 1000), true, 5));
    CHECK(jobs.size() == 16 && jobs.back().actions.front().kind == "text");
}

HMI_TEST(automatic_readback_never_replays_a_stale_final_or_speaks_during_capture)
{
    hmi::app::Pending pending;
    pending.transcript = {"已完成的录音。", true, -1.0f};
    pending.speech_generation = 7;
    hmi::app::CaptureGate gate;
    CHECK(hmi::app::transcript_readback_allowed(pending, gate, true, 7));
    CHECK(!hmi::app::transcript_readback_allowed(pending, gate, false, 7));
    // A newer recording or an off/on cycle cannot revive an older final.
    CHECK(!hmi::app::transcript_readback_allowed(pending, gate, true, 8));
    for (const auto phase : {hmi::app::CaptureGate::Phase::Opening,
                            hmi::app::CaptureGate::Phase::Recording,
                            hmi::app::CaptureGate::Phase::Processing})
    {
        gate.phase = phase;
        CHECK(!hmi::app::transcript_readback_allowed(pending, gate, true, 7));
    }
    gate.phase = hmi::app::CaptureGate::Phase::Idle;
    gate.requested = true;
    CHECK(!hmi::app::transcript_readback_allowed(pending, gate, true, 7));
    gate.requested = false;
    pending.transcript.final = false;
    CHECK(!hmi::app::transcript_readback_allowed(pending, gate, true, 7));
}

HMI_TEST(recording_terminal_outcomes_keep_identity_and_distinguish_skip_failure_and_cancel)
{
    hmi::app::Recording recording;
    recording.request_id = 901;
    const auto skipped = hmi::app::recording_terminal_event(recording, "early_release", "released", false, true);
    CHECK(skipped.request_id == 901 && skipped.ok && skipped.skipped && !skipped.cancelled);
    CHECK(skipped.stop_reason == "early_release" && skipped.at_ms == 0 && skipped.latency_ms < 0.0);
    const auto failed = hmi::app::recording_terminal_event(recording, "mic_open_failed", "not opened");
    CHECK(failed.request_id == 901 && !failed.ok && !failed.skipped && !failed.cancelled);
    CHECK(failed.stop_reason == "mic_open_failed" && failed.asr_ms < 0.0 && failed.tts_ms < 0.0);
    recording.released_at = hmi::app::milliseconds() - 5;
    const auto cancelled = hmi::app::recording_terminal_event(recording, "runtime_stopped", "stopped", true);
    CHECK(cancelled.request_id == 901 && cancelled.ok && cancelled.cancelled && !cancelled.skipped);
    CHECK(cancelled.stop_reason == "runtime_stopped" && cancelled.at_ms == recording.released_at && cancelled.latency_ms >= 5.0);
}

HMI_TEST(recording_discard_outcomes_preserve_the_actual_failure_reason)
{
    using Discard = hmi::app::Recording::Discard;
    CHECK(hmi::app::recording_discard_reason(Discard::Overrun) == "audio_overrun");
    CHECK(hmi::app::recording_discard_reason(Discard::Shortcut) == "shortcut_unavailable");
    CHECK(hmi::app::recording_discard_reason(Discard::Inference) == "asr_failed");
}
