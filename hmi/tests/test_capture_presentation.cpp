#include "check.hpp"
#include "app/capture_presentation.hpp"

#include <string_view>

HMI_TEST(capture_presentation_hides_startup_without_a_capture_request)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.state = hmi::app::VoiceState::Starting;
    snapshot.refining = true;
    snapshot.audio_seconds = 45.0;
    snapshot.processed_audio_seconds = 44.0;
    const auto presentation = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(presentation.phase == hmi::base::CapturePhase::Hidden);
    CHECK(presentation.request_id == 0 && presentation.captured_frames == 0);
}

HMI_TEST(capture_presentation_distinguishes_preparing_recording_and_real_processing)
{
    using Phase = hmi::base::CapturePhase;
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 51;
    snapshot.state = hmi::app::VoiceState::Starting;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).phase == Phase::Preparing);
    snapshot.state = hmi::app::VoiceState::Listening;
    snapshot.capture_active = true;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).phase == Phase::Recording);
    snapshot.captured_frames = 36480;
    snapshot.capture_active = false;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).phase == Phase::Processing);
    snapshot.state = hmi::app::VoiceState::Starting;
    snapshot.refining = true;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).phase == Phase::Processing);
}

HMI_TEST(capture_presentation_copies_only_true_capture_frames_and_rms)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 52;
    snapshot.state = hmi::app::VoiceState::Listening;
    snapshot.capture_active = true;
    snapshot.captured_frames = 101920;
    snapshot.recording_limit_seconds = 20;
    snapshot.capture_levels[0] = 0.125f;
    snapshot.capture_levels[63] = 0.25f;
    snapshot.capture_live_level = 0.375f;
    snapshot.audio_seconds = 800.0;
    snapshot.processed_audio_seconds = 700.0;
    snapshot.level = 0.875f;
    const auto presentation = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(presentation.phase == hmi::base::CapturePhase::Recording);
    CHECK(presentation.request_id == 52 && presentation.captured_frames == 101920 && presentation.limit_seconds == 20);
    CHECK(presentation.levels == snapshot.capture_levels && presentation.live_level == 0.375f);
    snapshot.audio_seconds = 0.0;
    snapshot.processed_audio_seconds = 0.0;
    snapshot.level = 0.0f;
    const auto changed_asr = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(changed_asr.captured_frames == presentation.captured_frames && changed_asr.levels == presentation.levels);
    CHECK(changed_asr.live_level == presentation.live_level);
}

HMI_TEST(capture_presentation_retains_the_release_prompt_after_microphone_stop)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 53;
    snapshot.captured_frames = 30 * 16000;
    snapshot.state = hmi::app::VoiceState::Stopped;
    snapshot.awaiting_release = true;
    const auto presentation = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(presentation.phase == hmi::base::CapturePhase::AwaitingRelease);
    CHECK(presentation.request_id == 53 && presentation.captured_frames == 30 * 16000);
    snapshot.awaiting_release = false;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).phase == hmi::base::CapturePhase::Hidden);
}

HMI_TEST(capture_presentation_hides_terminal_states_even_with_stale_activity)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 54;
    snapshot.captured_frames = 16000;
    snapshot.capture_active = true;
    snapshot.refining = true;
    for (const auto state : {hmi::app::VoiceState::Stopped, hmi::app::VoiceState::Error, hmi::app::VoiceState::Unavailable})
    {
        snapshot.state = state;
        CHECK(hmi::app::detail::make_capture_presentation(snapshot).phase == hmi::base::CapturePhase::Hidden);
    }
    snapshot.awaiting_release = true;
    for (const auto state : {hmi::app::VoiceState::Error, hmi::app::VoiceState::Unavailable})
    {
        snapshot.state = state;
        CHECK(hmi::app::detail::make_capture_presentation(snapshot).phase == hmi::base::CapturePhase::Hidden);
    }
    snapshot.state = hmi::app::VoiceState::Listening;
    snapshot.capture_request_id = 0;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).phase == hmi::base::CapturePhase::Hidden);
}

HMI_TEST(capture_presentation_does_not_keep_a_completed_capture_visible_for_tts)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 55;
    snapshot.captured_frames = 16000;
    snapshot.state = hmi::app::VoiceState::Starting;
    snapshot.refining = true;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).phase == hmi::base::CapturePhase::Processing);
    snapshot = {};
    snapshot.state = hmi::app::VoiceState::Stopped;
    snapshot.tts_busy = true;
    snapshot.audio_seconds = 1.0;
    const auto completed = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(completed.phase == hmi::base::CapturePhase::Hidden);
    CHECK(completed.request_id == 0 && completed.captured_frames == 0 && completed.live_level == 0.0f);
    CHECK((completed.levels == std::array<float, 64>{}));
}

HMI_TEST(capture_presentation_excludes_prior_text_before_capture_and_after_completion)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 60;
    snapshot.state = hmi::app::VoiceState::Starting;
    snapshot.last_transcript = "上一条已经输入的文字";
    snapshot.transcript_final = false;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).transcript[0] == '\0');
    snapshot.capture_active = true;
    snapshot.state = hmi::app::VoiceState::Listening;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).transcript[0] == '\0');
    snapshot.captured_frames = 16000;
    snapshot.transcript_final = true;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).transcript[0] == '\0');
    snapshot.capture_active = false;
    snapshot.transcript_final = false;
    for (const auto state : {hmi::app::VoiceState::Stopped, hmi::app::VoiceState::Error, hmi::app::VoiceState::Unavailable})
    {
        snapshot.state = state;
        const auto presentation = hmi::app::detail::make_capture_presentation(snapshot);
        CHECK(presentation.phase == hmi::base::CapturePhase::Hidden && presentation.transcript[0] == '\0');
    }
    snapshot.state = hmi::app::VoiceState::Listening;
    snapshot.capture_request_id = 0;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).transcript[0] == '\0');
}

HMI_TEST(capture_presentation_replaces_the_full_preview_and_clears_empty_text)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 61;
    snapshot.state = hmi::app::VoiceState::Listening;
    snapshot.capture_active = true;
    snapshot.captured_frames = 32000;
    snapshot.transcript_final = false;
    snapshot.last_transcript = "今天晚上";
    const auto first = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(std::string_view(first.transcript.data()) == "今天晚上");
    snapshot.last_transcript = "明天晚上会不会下雨？";
    snapshot.partial_revision = 2;
    const auto replacement = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(std::string_view(replacement.transcript.data()) == "明天晚上会不会下雨？");
    CHECK(first.transcript != replacement.transcript);
    snapshot.last_transcript.clear();
    CHECK((hmi::app::detail::make_capture_presentation(snapshot).transcript == std::array<char, 1024>{}));
}

HMI_TEST(capture_presentation_retains_current_preview_while_processing_and_final_while_awaiting_release)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 62;
    snapshot.state = hmi::app::VoiceState::Starting;
    snapshot.captured_frames = 48000;
    snapshot.transcript_final = false;
    snapshot.refining = true;
    snapshot.last_transcript = "正在整理的这句话";
    const auto processing = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(processing.phase == hmi::base::CapturePhase::Processing);
    CHECK(std::string_view(processing.transcript.data()) == snapshot.last_transcript);
    snapshot.transcript_final = true;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).transcript[0] == '\0');
    snapshot.state = hmi::app::VoiceState::Stopped;
    snapshot.refining = false;
    snapshot.awaiting_release = true;
    snapshot.last_transcript = "录满后保留的完整终稿。";
    const auto retained = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(retained.phase == hmi::base::CapturePhase::AwaitingRelease);
    CHECK(std::string_view(retained.transcript.data()) == snapshot.last_transcript);
    snapshot.awaiting_release = false;
    CHECK(hmi::app::detail::make_capture_presentation(snapshot).transcript[0] == '\0');
}

HMI_TEST(capture_presentation_converts_control_whitespace_without_changing_unicode)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 63;
    snapshot.state = hmi::app::VoiceState::Listening;
    snapshot.capture_active = true;
    snapshot.captured_frames = 64000;
    snapshot.transcript_final = false;
    snapshot.last_transcript = "第一行\r\n第二行\t😀";
    const auto presentation = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(std::string_view(presentation.transcript.data()) == "第一行  第二行 😀");
    CHECK(presentation.transcript.back() == '\0');
}

HMI_TEST(capture_presentation_retains_a_bounded_utf8_tail_at_codepoint_boundaries)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 64;
    snapshot.state = hmi::app::VoiceState::Listening;
    snapshot.capture_active = true;
    snapshot.captured_frames = 80000;
    snapshot.transcript_final = false;
    const std::string_view unit = "中😀";
    for (int index = 0; index < 200; ++index) snapshot.last_transcript += unit;
    const auto presentation = hmi::app::detail::make_capture_presentation(snapshot);
    const std::string_view tail(presentation.transcript.data());
    CHECK(tail.size() <= presentation.transcript.size() - 1);
    CHECK(tail.size() > presentation.transcript.size() - unit.size() - 1);
    CHECK(!tail.empty() && (static_cast<unsigned char>(tail.front()) & 0xc0U) != 0x80U);
    CHECK(snapshot.last_transcript.ends_with(tail));
    CHECK(presentation.transcript[tail.size()] == '\0' && presentation.transcript.back() == '\0');
    CHECK(tail.ends_with(unit));
    CHECK(tail == snapshot.last_transcript.substr(snapshot.last_transcript.size() - tail.size()));
}

HMI_TEST(capture_presentation_uses_the_entire_payload_capacity_and_terminates)
{
    hmi::app::VoiceSnapshot snapshot;
    snapshot.capture_request_id = 65;
    snapshot.state = hmi::app::VoiceState::Listening;
    snapshot.capture_active = true;
    snapshot.captured_frames = 96000;
    snapshot.transcript_final = false;
    snapshot.last_transcript = std::string(1023, 'a');
    const auto exact = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(std::string_view(exact.transcript.data()) == snapshot.last_transcript);
    CHECK(exact.transcript.back() == '\0');
    snapshot.last_transcript = std::string(800, 'b') + std::string(1023, 'a');
    const auto truncated = hmi::app::detail::make_capture_presentation(snapshot);
    CHECK(std::string_view(truncated.transcript.data()) == std::string(1023, 'a'));
    CHECK(truncated.transcript.back() == '\0');
}
