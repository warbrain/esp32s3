#pragma once
#include <functional>
#include <string_view>

namespace hmi::app { struct VoiceSnapshot; }

namespace hmi::ui
{
void DrawVoiceHistory(const app::VoiceSnapshot &snapshot,
                      const std::function<void(std::string_view)> &replay_speech = {}, float height = 0.0f);
void DrawHistoryPage(const app::VoiceSnapshot &snapshot,
                   const std::function<void(std::string_view)> &replay_speech = {});
}
