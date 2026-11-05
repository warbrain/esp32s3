#include "ui/pages/history_page.hpp"
#include "app/voice_runtime.hpp"
#include "imgui.h"
#include "ui/theme.hpp"
#include "app/voice_history.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
namespace hmi::ui
{
namespace
{
std::string recorded_time(std::int64_t recorded_at_ms)
{
    if (recorded_at_ms <= 0) return "—";
    using namespace std::chrono;
    // Display Asia/Shanghai independently of the operating system's timezone.
    const auto timestamp = sys_time<milliseconds>{milliseconds{recorded_at_ms}} + hours{8};
    const auto day = floor<days>(timestamp);
    const year_month_day date{day};
    const hh_mm_ss time{floor<seconds>(timestamp - day)};
    if (!date.ok()) return "—";
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04d-%02u-%02u %02u:%02u:%02u",
        static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
        static_cast<unsigned>(time.hours().count()), static_cast<unsigned>(time.minutes().count()),
        static_cast<unsigned>(time.seconds().count()));
    return text;
}

std::string duration_text(double milliseconds)
{
    if (!std::isfinite(milliseconds) || milliseconds < 0.0) return "—";
    char text[48]{};
    if (milliseconds < 1000.0)
        std::snprintf(text, sizeof(text), "%.0f 毫秒", milliseconds);
    else
        std::snprintf(text, sizeof(text), "%.2f 秒", milliseconds / 1000.0);
    return text;
}

const char *action_name(const app::VoiceEvent &event, bool speech)
{
    if (speech) return "朗读";
    if (event.action == "text") return "输入文字";
    if (event.action == "launch") return "打开应用";
    if (event.action == "key") return "快捷操作";
    if (event.action == "wait") return "等待";
    return "执行操作";
}
}

void DrawVoiceHistory(const app::VoiceSnapshot &snapshot,
                      const std::function<void(std::string_view)> &replay_speech, float height)
{
    const float scale = UiScale();
    const float area_height = height > 0.0f ? height : ImGui::GetContentRegionAvail().y;
    const float toolbar_y = ImGui::GetCursorPosY();
    ImGui::BeginDisabled(snapshot.recent.empty());
    const bool scroll_up = ImGui::Button("向上");
    ImGui::SameLine();
    const bool scroll_down = ImGui::Button("向下");
    ImGui::EndDisabled();
    const float toolbar_height = ImGui::GetCursorPosY() - toolbar_y;
    const float table_height = std::max(1.0f, area_height - toolbar_height);
    float date_width = ImGui::CalcTextSize("日期时间").x;
    float action_width = ImGui::CalcTextSize("执行操作").x;
    float asr_width = ImGui::CalcTextSize("识别耗时").x;
    float tts_width = ImGui::CalcTextSize("朗读耗时").x;
    for (const auto &event : snapshot.recent)
    {
        const bool speech = event.action == "speak" || !event.speech_text.empty();
        date_width = std::max(date_width, ImGui::CalcTextSize(recorded_time(event.recorded_at_ms).c_str()).x);
        action_width = std::max(action_width, ImGui::CalcTextSize(action_name(event, speech)).x);
        asr_width = std::max(asr_width, ImGui::CalcTextSize(duration_text(event.asr_ms).c_str()).x);
        tts_width = std::max(tts_width, ImGui::CalcTextSize(duration_text(event.tts_ms).c_str()).x);
    }
    const float text_padding = 8.0f * scale;
    date_width += text_padding;
    action_width += text_padding;
    asr_width += text_padding;
    tts_width += text_padding;
    const float status_width = std::max(120.0f * scale, ImGui::CalcTextSize("已完成").x +
        ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize("重播").x +
        ImGui::GetStyle().FramePadding.x * 2.0f + text_padding);
    const float inner_width = std::max(ImGui::GetContentRegionAvail().x, date_width + action_width +
        asr_width + tts_width + status_width + 200.0f * scale + ImGui::GetStyle().CellPadding.x * 12.0f);
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("voice_history_rows", 6, flags, ImVec2(0, table_height), inner_width))
    {
        const float scroll_y = ImGui::GetScrollY();
        const float scroll_max = ImGui::GetScrollMaxY();
        const bool follow_latest = scroll_y >= scroll_max - 1.0f;
        const float page_height = std::max(ImGui::GetTextLineHeightWithSpacing(),
            table_height - ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ScrollbarSize);
        ImGui::TableSetupColumn("日期时间", ImGuiTableColumnFlags_WidthFixed, date_width);
        ImGui::TableSetupColumn("动作", ImGuiTableColumnFlags_WidthFixed, action_width);
        ImGui::TableSetupColumn("内容", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("识别耗时", ImGuiTableColumnFlags_WidthFixed, asr_width);
        ImGui::TableSetupColumn("朗读耗时", ImGuiTableColumnFlags_WidthFixed, tts_width);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, status_width);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        if (snapshot.recent.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("还没有记录。");
        }
        const bool unavailable = snapshot.state == app::VoiceState::Starting ||
            snapshot.state == app::VoiceState::Error || snapshot.state == app::VoiceState::Unavailable;
        const bool replay_disabled = !replay_speech || !snapshot.tts_enabled || !snapshot.tts_available ||
            !snapshot.engine_ready || !snapshot.model_ready || snapshot.tts_busy ||
            snapshot.state == app::VoiceState::Listening || snapshot.refining || snapshot.awaiting_release || unavailable;
        float content_bottom = 0.0f;
        for (const auto &event : snapshot.recent)
        {
            const std::string identity = std::to_string(event.sequence);
            ImGui::PushID(identity.c_str());
            const bool speech = event.action == "speak" || !event.speech_text.empty();
            const bool input_status = app::detail::voice_event_has_input_delivery(event);
            const bool interrupted = event.cancelled || event.skipped ||
                (input_status && event.input_delivery == "unknown");
            const char *result = app::detail::voice_event_status(event);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(recorded_time(event.recorded_at_ms).c_str());
            content_bottom = std::max(content_bottom, ImGui::GetItemRectMax().y);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(action_name(event, speech));
            content_bottom = std::max(content_bottom, ImGui::GetItemRectMax().y);
            ImGui::TableSetColumnIndex(2);
            const auto &text = speech && !event.speech_text.empty() ? event.speech_text : event.transcript;
            ImGui::TextWrapped("%s", text.empty() ? "—" : text.c_str());
            content_bottom = std::max(content_bottom, ImGui::GetItemRectMax().y);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(duration_text(event.asr_ms).c_str());
            if (event.asr_ms >= 0.0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("把这段录音变成文字所用的时间，包含说话时的更新和结束后的整理。");
            content_bottom = std::max(content_bottom, ImGui::GetItemRectMax().y);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(duration_text(event.tts_ms).c_str());
            if (event.tts_ms >= 0.0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", event.cancelled ? "停止时已完成部分的处理时间，不含实际播放时间。" :
                    "生成朗读声音所用的时间，不含实际播放时间。");
            content_bottom = std::max(content_bottom, ImGui::GetItemRectMax().y);
            ImGui::TableSetColumnIndex(5);
            ImGui::PushStyleColor(ImGuiCol_Text, interrupted ? ImVec4(1.0f, .82f, .48f, 1.0f) :
                event.ok ? ImVec4(.65f, .89f, .76f, 1.0f) : ImVec4(1.0f, .70f, .62f, 1.0f));
            ImGui::TextUnformatted(result);
            ImGui::PopStyleColor();
            if (input_status && ImGui::IsItemHovered())
            {
                const char *explanation = event.input_delivery == "verified" ? "已核对输入位置附近的文字。" :
                    event.input_delivery == "delivered_unverified" ? "文字已交给目标应用，尚未核对输入框内容。" :
                    event.input_delivery == "mismatch" ? "目标应用中的文字与本次输入不一致，未自动重输。" :
                    event.input_delivery == "unknown" ? "目标应用的输入结果暂时无法确认，未自动重输。" : "本次没有提交文字。";
                ImGui::SetTooltip("%s", explanation);
            }
            if (speech && !event.speech_text.empty())
            {
                ImGui::SameLine();
                ImGui::BeginDisabled(replay_disabled);
                if (ImGui::SmallButton("重播")) replay_speech(event.speech_text);
                ImGui::EndDisabled();
            }
            content_bottom = std::max(content_bottom, ImGui::GetItemRectMax().y);
            ImGui::PopID();
        }
        if (scroll_up)
            ImGui::SetScrollY(std::max(0.0f, scroll_y - page_height));
        else if (scroll_down && scroll_y + page_height < scroll_max)
            ImGui::SetScrollY(scroll_y + page_height);
        else if (!snapshot.recent.empty() && (follow_latest || scroll_down))
            ImGui::SetScrollFromPosY(content_bottom - ImGui::GetWindowPos().y + ImGui::GetStyle().CellPadding.y, 1.0f);
        ImGui::EndTable();
    }
}

void DrawHistoryPage(const app::VoiceSnapshot &snapshot,
                   const std::function<void(std::string_view)> &replay_speech)
{
    ImGui::TextUnformatted("最近记录");
    ImGui::Spacing();
    DrawVoiceHistory(snapshot, replay_speech);
}
}
