#include "ui/pages/voice_page.hpp"
#include "app/voice_runtime.hpp"
#include "ui/pages/history_page.hpp"
#include "ui/theme.hpp"
#include "imgui.h"
#include "app/capture_presentation.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hmi::ui
{
namespace
{
float capture_amount(float level)
{
    const float rms = std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
    const float decibels = 20.0f * std::log10(std::max(rms, 0.000001f));
    return std::clamp((decibels + 72.0f) / 60.0f, 0.0f, 1.0f);
}

void draw_capture_microphone(ImDrawList *draw, ImVec2 center, float scale, ImU32 color)
{
    draw->AddRectFilled(ImVec2(center.x - 2.5f * scale, center.y - 7.0f * scale),
        ImVec2(center.x + 2.5f * scale, center.y + 2.0f * scale), color, 2.5f * scale);
    draw->AddBezierCubic(ImVec2(center.x - 5.0f * scale, center.y - 1.0f * scale),
        ImVec2(center.x - 5.0f * scale, center.y + 7.0f * scale),
        ImVec2(center.x + 5.0f * scale, center.y + 7.0f * scale),
        ImVec2(center.x + 5.0f * scale, center.y - 1.0f * scale), color, 1.5f * scale);
    draw->AddLine(ImVec2(center.x, center.y + 5.0f * scale),
        ImVec2(center.x, center.y + 9.0f * scale), color, 1.5f * scale);
    draw->AddLine(ImVec2(center.x - 3.0f * scale, center.y + 9.0f * scale),
        ImVec2(center.x + 3.0f * scale, center.y + 9.0f * scale), color, 1.5f * scale);
}

void draw_capture_text(ImDrawList *draw, const char *value, ImVec2 position, float width, ImU32 color)
{
    if (width <= 0.0f) return;
    const char *end = value;
    while (*end) ++end;
    const char *first = value;
    if (ImGui::CalcTextSize(value, end).x > width)
    {
        constexpr const char *ellipsis = "…";
        const float ellipsis_width = ImGui::CalcTextSize(ellipsis).x;
        if (width <= ellipsis_width)
        {
            draw->AddText(position, color, ellipsis);
            return;
        }
        const float remaining = width - ellipsis_width;
        const auto length = static_cast<std::size_t>(end - value);
        std::size_t low = 0, high = length;
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            std::size_t boundary = middle;
            while (boundary < length && (static_cast<unsigned char>(value[boundary]) & 0xc0U) == 0x80U) ++boundary;
            if (ImGui::CalcTextSize(value + boundary, end).x <= remaining) high = middle;
            else low = middle + 1;
        }
        while (low < length && (static_cast<unsigned char>(value[low]) & 0xc0U) == 0x80U) ++low;
        first = value + low;
        draw->AddText(position, color, ellipsis);
        position.x += ellipsis_width;
    }
    draw->AddText(position, color, first, end);
}

void draw_capture_activity(const app::VoiceSnapshot &snapshot, VoicePageState &state, float scale)
{
    if (state.capture_request_id != snapshot.capture_request_id)
    {
        state.capture_request_id = snapshot.capture_request_id;
        state.levels.fill(0.0f);
    }
    const float delta = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    for (std::size_t index = 0; index < state.levels.size(); ++index)
    {
        const float level = index + 1 < state.levels.size() ? snapshot.capture_levels[index + 1] : snapshot.capture_live_level;
        const float target = capture_amount(level);
        const float rate = target > state.levels[index] ? 28.0f : 9.0f;
        // Interpolation changes presentation only; every target is captured RMS.
        if (snapshot.capture_active) state.levels[index] += (target - state.levels[index]) * (1.0f - std::exp(-rate * delta));
        else state.levels[index] = target;
    }
    const double limit = static_cast<double>(std::max(1, snapshot.recording_limit_seconds));
    const double seconds = std::clamp(static_cast<double>(snapshot.captured_frames) / 16000.0, 0.0, limit);
    const float progress = static_cast<float>(seconds / limit);
    const bool finishing = !snapshot.capture_active || limit - seconds <= 5.0;
    const ImVec4 accent = finishing ? ImVec4(1.0f, .81f, .48f, 1.0f) : ImVec4(.48f, .90f, .74f, 1.0f);
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = 88.0f * scale;
    const float center_y = position.y + 23.0f * scale;
    const float text_height = ImGui::GetTextLineHeight();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(position, ImVec2(position.x + width, position.y + height),
        ImGui::GetColorU32(ImVec4(.08f, .12f, .18f, .88f)), 12.0f * scale);
    draw->AddRect(position, ImVec2(position.x + width, position.y + height),
        ImGui::GetColorU32(ImVec4(.55f, .74f, .94f, .18f)), 12.0f * scale);
    const float breath = snapshot.capture_active ? .5f + .5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.2f) : 0.0f;
    const ImVec2 indicator(position.x + 20.0f * scale, center_y);
    ImVec4 glow = accent;
    glow.w = .09f + .09f * breath;
    draw->AddCircleFilled(indicator, (6.0f + 2.0f * breath) * scale, ImGui::GetColorU32(glow), 24);
    draw_capture_microphone(draw, indicator, scale, ImGui::GetColorU32(accent));
    const int elapsed = static_cast<int>(seconds);
    const int maximum = static_cast<int>(limit);
    char clock[64]{};
    std::snprintf(clock, sizeof(clock), "%02d:%02d / %02d:%02d", elapsed / 60, elapsed % 60, maximum / 60, maximum % 60);
    const float clock_width = ImGui::CalcTextSize(clock).x;
    const char *reading = snapshot.capture_active ? "正在采集" : "请松开快捷键";
    const float reading_width = ImGui::CalcTextSize(reading).x;
    const bool show_clock = width > clock_width + reading_width + 72.0f * scale;
    draw->PushClipRect(position, ImVec2(position.x + width, position.y + height), true);
    draw->AddText(ImVec2(position.x + 35.0f * scale, center_y - text_height * .5f), ImGui::GetColorU32(ImGuiCol_Text), reading);
    if (show_clock)
        draw->AddText(ImVec2(position.x + width - clock_width - 14.0f * scale, center_y - text_height * .5f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), clock);
    const float wave_x = position.x + 48.0f * scale + reading_width;
    const float wave_end = position.x + width - (show_clock ? clock_width + 28.0f * scale : 14.0f * scale);
    const float wave_width = wave_end - wave_x;
    if (wave_width > 18.0f * scale)
    {
        const int columns = std::clamp(static_cast<int>(wave_width / (6.0f * scale)), 3, 64);
        const float pitch = 6.0f * scale;
        const float first_x = wave_x + (wave_width - (static_cast<float>(columns) - .5f) * pitch) * .5f;
        for (int index = 0; index < columns; ++index)
        {
            const auto sample = state.levels.size() - static_cast<std::size_t>(columns) + static_cast<std::size_t>(index);
            const float bar_height = (3.0f + state.levels[sample] * 21.0f) * scale;
            const float x = first_x + static_cast<float>(index) * pitch;
            ImVec4 color = accent;
            color.w = .25f + .65f * (static_cast<float>(index) + 1.0f) / static_cast<float>(columns);
            draw->AddRectFilled(ImVec2(x, center_y - bar_height * .5f), ImVec2(x + 3.0f * scale, center_y + bar_height * .5f),
                ImGui::GetColorU32(color), 1.5f * scale);
        }
    }
    const float track_width = std::max(0.0f, width - 28.0f * scale);
    const float track_height = 28.0f * scale;
    const ImVec2 track(position.x + 14.0f * scale, position.y + 47.0f * scale);
    const ImVec2 track_end(track.x + track_width, track.y + track_height);
    draw->AddRectFilled(track, track_end, ImGui::GetColorU32(ImVec4(.12f, .17f, .23f, .95f)), 6.0f * scale);
    if (progress > 0.0f)
    {
        ImVec4 fill = accent;
        fill.w = .13f;
        draw->AddRectFilled(track, ImVec2(track.x + track_width * progress, track_end.y),
            ImGui::GetColorU32(fill), 6.0f * scale);
    }
    const auto presentation = app::detail::make_capture_presentation(snapshot);
    const bool has_text = presentation.transcript.front() != '\0';
    const char *transcript = has_text ? presentation.transcript.data() : "说话后，文字会显示在这里";
    const ImVec2 text_start(track.x + 10.0f * scale, track.y + (track_height - text_height) * .5f);
    const ImVec2 text_end(track_end.x - 10.0f * scale, track_end.y);
    if (text_end.x > text_start.x)
    {
        draw->PushClipRect(ImVec2(text_start.x, track.y), text_end, true);
        draw_capture_text(draw, transcript, text_start, text_end.x - text_start.x,
            ImGui::GetColorU32(has_text ? ImGuiCol_Text : ImGuiCol_TextDisabled));
        draw->PopClipRect();
    }
    draw->PopClipRect();
    ImGui::Dummy(ImVec2(width, height));
}

void draw_processing_activity(float scale)
{
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float time = static_cast<float>(ImGui::GetTime());
    ImDrawList *draw = ImGui::GetWindowDrawList();
    for (int index = 0; index < 3; ++index)
    {
        const float pulse = 0.5f + 0.5f * std::sin(time * 4.0f - static_cast<float>(index) * 0.9f);
        draw->AddCircleFilled(ImVec2(position.x + (6.0f + static_cast<float>(index) * 12.0f) * scale,
                position.y + 6.0f * scale),
            (2.5f + pulse) * scale, ImGui::GetColorU32(ImVec4(.64f, .79f, 1.0f, .25f + .75f * pulse)), 16);
    }
    ImGui::Dummy(ImVec2(36.0f * scale, 12.0f * scale));
}

void draw_speech_controls(const app::VoiceSnapshot &snapshot, const std::function<void()> &test_speech,
                          const std::function<bool(bool)> &set_speech_enabled)
{
    const float scale = UiScale();
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const ImVec2 size(44.0f * scale, 24.0f * scale);
    bool enabled = snapshot.tts_enabled;
    ImGuiStorage *storage = ImGui::GetStateStorage();
    const ImGuiID armed_id = ImGui::GetID("tts_enabled_armed");
    const ImGuiID dragged_id = ImGui::GetID("tts_enabled_dragged");
    const ImGuiID position_id = ImGui::GetID("tts_enabled_position");
    const ImGuiID press_x_id = ImGui::GetID("tts_enabled_press_x");
    const ImGuiID press_y_id = ImGui::GetID("tts_enabled_press_y");
    bool armed = storage->GetBool(armed_id);
    bool dragged = storage->GetBool(dragged_id);
    float drag_x = storage->GetFloat(position_id, position.x);
    float press_x = storage->GetFloat(press_x_id, position.x);
    float press_y = storage->GetFloat(press_y_id, position.y);
    ImGui::BeginDisabled(!set_speech_enabled);
    const bool clicked = ImGui::InvisibleButton("tts_enabled", size);
    if (!set_speech_enabled)
    {
        armed = dragged = false;
    }
    else
    {
        if (ImGui::IsItemActivated())
        {
            armed = true;
            dragged = false;
            press_x = ImGui::GetIO().MousePos.x;
            press_y = ImGui::GetIO().MousePos.y;
        }
        if (armed && ImGui::IsMousePosValid())
        {
            drag_x = ImGui::GetIO().MousePos.x;
            const float dx = drag_x - press_x;
            const float dy = ImGui::GetIO().MousePos.y - press_y;
            const float distance = dx * dx + dy * dy;
            const float threshold = ImGui::GetIO().MouseDragThreshold;
            if (distance > 0.0f && distance >= threshold * threshold) dragged = true;
        }
        // Retain the drag across release, including movement in the release frame.
        const bool released = armed && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        if (released)
        {
            if (clicked || dragged)
            {
                const bool requested = dragged ? drag_x >= position.x + size.x * 0.5f : !enabled;
                if (requested != enabled && set_speech_enabled(requested)) enabled = requested;
            }
            armed = dragged = false;
        }
        else if (armed && !ImGui::IsItemActive())
            armed = dragged = false;
    }
    storage->SetBool(armed_id, armed);
    storage->SetBool(dragged_id, dragged);
    storage->SetFloat(position_id, drag_x);
    storage->SetFloat(press_x_id, press_x);
    storage->SetFloat(press_y_id, press_y);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec4 track = enabled ? ImVec4(.28f, .66f, .53f, 1.0f) : ImVec4(.25f, .30f, .38f, 1.0f);
    draw->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y),
        ImGui::GetColorU32(track), size.y * 0.5f);
    float thumb = enabled ? size.x - size.y * 0.5f : size.y * 0.5f;
    if (armed && dragged)
        thumb = std::clamp(drag_x - position.x, size.y * 0.5f, size.x - size.y * 0.5f);
    draw->AddCircleFilled(ImVec2(position.x + thumb,
            position.y + size.y * 0.5f), size.y * 0.5f - 3.0f * scale,
        ImGui::GetColorU32(ImVec4(.92f, .95f, .98f, 1.0f)), 24);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted("自动朗读");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("开启后读出转写文字；开始录音或关闭开关会停止朗读。");
    const bool recording = snapshot.state == app::VoiceState::Listening;
    const bool unavailable = snapshot.state == app::VoiceState::Starting ||
        snapshot.state == app::VoiceState::Error || snapshot.state == app::VoiceState::Unavailable;
    const bool disabled = !enabled || !test_speech || !snapshot.engine_ready || !snapshot.model_ready ||
        !snapshot.tts_available || snapshot.tts_busy || recording || unavailable ||
        snapshot.refining || snapshot.awaiting_release;
    ImGui::SameLine(0.0f, 20.0f * scale);
    ImGui::BeginDisabled(disabled);
    if (ImGui::Button("试听") && test_speech)
        test_speech();
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.0f);
    if (!snapshot.tts_toggle_error.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, .70f, .62f, 1.0f), "未能保存朗读开关，请再试一次。");
    }
    else if (enabled && !snapshot.tts_error.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, .70f, .62f, 1.0f), "这段朗读未能完成，可以稍后重试。");
    }
    else if (enabled && snapshot.tts_busy)
    {
        ImGui::TextDisabled("%s", snapshot.tts_first_audio_ms > 0.0 ? "正在朗读…" : "正在准备朗读，请稍候…");
    }
    else if (enabled && !snapshot.tts_available)
    {
        ImGui::TextDisabled("朗读暂不可用，请稍后重试。");
    }
    else if (enabled)
    {
        for (auto event = snapshot.recent.rbegin(); event != snapshot.recent.rend(); ++event)
        {
            if (event->speech_text.empty()) continue;
            if (event->cancelled)
                ImGui::TextDisabled("%s", recording ? "开始录音，已停止上一段朗读。" : "上一段朗读已停止，可在最近记录中重播。");
            break;
        }
    }
    ImGui::PopTextWrapPos();
}
}

void DrawVoicePage(const app::VoiceSnapshot &snapshot, VoicePageState &state, const std::function<void()> &test_speech,
                   const std::function<bool(bool)> &set_speech_enabled,
                   const std::function<void(std::string_view)> &replay_speech,
                   const std::function<void()> &select_microphone)
{
    const float s = UiScale();
    const bool listening = snapshot.capture_active;
    const bool processing = !listening && !snapshot.awaiting_release &&
        (snapshot.refining || snapshot.state == app::VoiceState::Starting || snapshot.state == app::VoiceState::Listening);
    const bool failed = snapshot.state == app::VoiceState::Error || snapshot.state == app::VoiceState::Unavailable;
    const char *heading = failed ? "暂时无法使用语音" :
        snapshot.awaiting_release ? "这段已录好，请结束录音" :
        listening ? "正在听，请说话…" :
        snapshot.refining || (processing && snapshot.state != app::VoiceState::Starting) ? "正在整理文字，请稍候…" :
        snapshot.state == app::VoiceState::Starting ? "正在准备，请稍候…" :
        snapshot.engine_ready && snapshot.model_ready ? "可以开始说话" : "语音已停止";
    const ImVec4 tint = failed ? ImVec4(1.0f, .62f, .58f, 1.0f) :
        snapshot.refining || snapshot.awaiting_release ? ImVec4(1.0f, .81f, .48f, 1.0f) : listening ?
        ImVec4(.48f, .90f, .74f, 1.0f) : ImVec4(.64f, .79f, 1.0f, 1.0f);

    const float microphone_width = std::min(278.0f * s, ImGui::GetContentRegionAvail().x * 0.48f);
    const bool header_table = select_microphone && ImGui::BeginTable("overview_header", 2);
    if (header_table)
    {
        ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("microphone", ImGuiTableColumnFlags_WidthFixed, microphone_width);
        ImGui::TableNextColumn();
    }
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
    ImGui::PushStyleColor(ImGuiCol_Text, tint);
    ImGui::TextWrapped("%s", heading);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (header_table)
    {
        ImGui::TableNextColumn();
        select_microphone();
        ImGui::EndTable();
    }
    if (failed)
        ImGui::TextWrapped("请检查麦克风连接；如果仍然无法使用，请重新打开应用。");
    else if (snapshot.awaiting_release)
    {
        draw_capture_activity(snapshot, state, s);
        ImGui::TextWrapped("松开说话快捷键后输入文字；连续录音时，请再按一次结束。");
    }
    else if (snapshot.capture_active)
    {
        draw_capture_activity(snapshot, state, s);
    }
    else if (processing)
        draw_processing_activity(s);
    else
        ImGui::TextDisabled("按住说话快捷键，松开后输入文字。");

    ImGui::Spacing();
    const float available_height = ImGui::GetContentRegionAvail().y;
    const float history_height = std::clamp(available_height * 0.36f, 100.0f * s, 220.0f * s);
    const bool history_open = ImGui::GetStateStorage()->GetInt(ImGui::GetID("最近记录"), 0) != 0;
    const float transcript_height = std::max(120.0f * s,
        available_height - 88.0f * s - (history_open ? history_height : 0.0f));
    if (ImGui::BeginChild("voice_transcript", ImVec2(0, transcript_height), ImGuiChildFlags_Borders))
    {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
        if (snapshot.last_transcript.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("你说的话会显示在这里。");
            ImGui::PopStyleColor();
        }
        else
            ImGui::TextWrapped("%s", snapshot.last_transcript.c_str());
        ImGui::PopFont();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    draw_speech_controls(snapshot, test_speech, set_speech_enabled);
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("最近记录"))
        DrawVoiceHistory(snapshot, replay_speech, history_height);
}
}
