#pragma once
#include <functional>
#include <array>
#include <cstdint>
#include <string_view>

namespace hmi::app { struct VoiceSnapshot; }

namespace hmi::ui
{
struct VoicePageState
{
    std::array<float, 64> levels{};
    std::uint64_t capture_request_id = 0;
};

void DrawVoicePage(const app::VoiceSnapshot &snapshot, VoicePageState &state,
                   const std::function<void()> &test_speech = {},
                   const std::function<bool(bool)> &set_speech_enabled = {},
                   const std::function<void(std::string_view)> &replay_speech = {},
                   const std::function<void()> &select_microphone = {});
}
