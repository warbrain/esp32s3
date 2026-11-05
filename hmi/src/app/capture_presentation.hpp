#pragma once

#include "base/capture_presentation.hpp"
#include "app/voice_runtime.hpp"

namespace hmi::app::detail
{
inline base::CapturePresentation make_capture_presentation(const VoiceSnapshot &snapshot) noexcept
{
    base::CapturePresentation result;
    result.request_id = snapshot.capture_request_id;
    result.captured_frames = snapshot.captured_frames;
    result.limit_seconds = snapshot.recording_limit_seconds;
    result.levels = snapshot.capture_levels;
    result.live_level = snapshot.capture_live_level;

    if (snapshot.capture_request_id == 0 || snapshot.state == VoiceState::Error ||
        snapshot.state == VoiceState::Unavailable) return result;
    // A retained final transcript waits for release with VoiceState::Stopped.
    if (snapshot.awaiting_release) result.phase = base::CapturePhase::AwaitingRelease;
    else if (snapshot.state == VoiceState::Stopped) return result;
    else if (snapshot.capture_active) result.phase = base::CapturePhase::Recording;
    else if (snapshot.state == VoiceState::Starting || snapshot.state == VoiceState::Listening || snapshot.refining)
        result.phase = snapshot.captured_frames == 0 ? base::CapturePhase::Preparing : base::CapturePhase::Processing;
    if (result.phase != base::CapturePhase::Hidden && result.phase != base::CapturePhase::Preparing &&
        snapshot.captured_frames != 0 && (!snapshot.transcript_final || snapshot.awaiting_release))
    {
        const auto &source = snapshot.last_transcript;
        const std::size_t capacity = result.transcript.size() - 1;
        std::size_t first = source.size() > capacity ? source.size() - capacity : 0;
        // Never begin the retained tail inside a UTF-8 code point.
        while (first < source.size() && (static_cast<unsigned char>(source[first]) & 0xc0U) == 0x80U) ++first;
        for (std::size_t index = first; index < source.size(); ++index)
        {
            const char value = source[index];
            result.transcript[index - first] = value == '\r' || value == '\n' || value == '\t' ? ' ' : value;
        }
    }
    return result;
}
}
