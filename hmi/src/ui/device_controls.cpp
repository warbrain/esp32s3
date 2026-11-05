#include "ui/device_controls.hpp"

#include "app/services.hpp"
#include "imgui.h"
#include "ui/theme.hpp"

#include <algorithm>
#include <string>

namespace hmi::ui
{
namespace
{
constexpr float kPickerWidth = 278.0f;

void DrawMicrophoneGlyph(ImDrawList *draw, ImVec2 center, ImU32 color)
{
    const float s = UiScale();
    draw->AddRectFilled(ImVec2(center.x - 3.5f * s, center.y - 10.0f * s),
                        ImVec2(center.x + 3.5f * s, center.y + 3.0f * s), color, 3.5f * s);
    draw->PathArcTo(ImVec2(center.x, center.y + 1.0f * s), 8.0f * s, 0.0f, 3.14159f, 20);
    draw->PathStroke(color, false, 1.8f * s);
    draw->AddLine(ImVec2(center.x, center.y + 9.0f * s), ImVec2(center.x, center.y + 12.0f * s), color, 1.8f * s);
    draw->AddLine(ImVec2(center.x - 4.0f * s, center.y + 12.0f * s),
                  ImVec2(center.x + 4.0f * s, center.y + 12.0f * s), color, 1.8f * s);
}
}

void DrawMicrophonePicker(hmi::audio::InputDevices &microphones)
{
    const float s = UiScale();
    const float trigger_width = std::min(kPickerWidth * s, std::max(1.0f, ImGui::GetContentRegionAvail().x));
    const ImVec2 trigger_position = ImGui::GetCursorScreenPos();
    ImDrawList *trigger_draw = ImGui::GetWindowDrawList();

    const auto &devices = microphones.devices();
    const auto selected = std::find_if(devices.begin(), devices.end(),
                                       [&microphones](const hmi::audio::InputDevice &device) {
                                           return device.id == microphones.selected_id();
                                       });
    const std::string selected_name = selected != devices.end()
                                          ? ToUtf8(selected->name)
                                          : devices.empty() ? std::string("未找到麦克风") : ToUtf8(devices.front().name);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f * s, 16.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 25.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 10.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * s, 6.0f * s));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(.12f, .17f, .24f, .88f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(.20f, .29f, .42f, .94f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(.24f, .36f, .52f, .98f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(.075f, .095f, .14f, .98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.63f, .76f, .96f, .24f));

    ImGui::SetNextWindowSizeConstraints(ImVec2(trigger_width, 0.0f), ImVec2(trigger_width, 360.0f * s));
    ImGui::SetNextItemWidth(trigger_width);
    const bool popup_open = ImGui::BeginCombo("##overview_microphone", "", ImGuiComboFlags_NoArrowButton);

    const float trigger_height = ImGui::GetFrameHeight();
    trigger_draw->AddRectFilled(ImVec2(trigger_position.x + 4.0f * s, trigger_position.y + 2.0f * s),
                                ImVec2(trigger_position.x + trigger_width - 4.0f * s, trigger_position.y + 19.0f * s),
                                IM_COL32(198, 220, 250, 14), 20.0f * s, ImDrawFlags_RoundCornersTop);
    trigger_draw->AddRect(trigger_position,
                          ImVec2(trigger_position.x + trigger_width, trigger_position.y + trigger_height),
                          IM_COL32(179, 207, 244, 72), 25.0f * s, 0, 1.0f * s);

    const ImVec2 icon_center(trigger_position.x + 28.0f * s, trigger_position.y + trigger_height * 0.5f);
    DrawMicrophoneGlyph(trigger_draw, icon_center, IM_COL32(192, 211, 238, 245));
    const ImVec2 text_min(trigger_position.x + 52.0f * s, trigger_position.y + 1.0f * s);
    const ImVec2 text_max(trigger_position.x + trigger_width - 34.0f * s, trigger_position.y + trigger_height - 1.0f * s);
    trigger_draw->PushClipRect(text_min, text_max, true);
    const ImVec2 name_size = ImGui::CalcTextSize(selected_name.c_str());
    trigger_draw->AddText(ImVec2(text_min.x, trigger_position.y + (trigger_height - name_size.y) * 0.5f),
                          devices.empty() ? IM_COL32(160, 173, 194, 255) : IM_COL32(235, 242, 252, 255),
                          selected_name.c_str());
    trigger_draw->PopClipRect();
    const float arrow_x = trigger_position.x + trigger_width - 18.0f * s;
    const float arrow_y = trigger_position.y + trigger_height * 0.5f;
    trigger_draw->AddLine(ImVec2(arrow_x - 4.0f * s, arrow_y - 2.0f * s), ImVec2(arrow_x, arrow_y + 2.0f * s),
                          IM_COL32(194, 211, 238, 230), 1.8f * s);
    trigger_draw->AddLine(ImVec2(arrow_x, arrow_y + 2.0f * s), ImVec2(arrow_x + 4.0f * s, arrow_y - 2.0f * s),
                          IM_COL32(194, 211, 238, 230), 1.8f * s);

    if (popup_open)
    {
        if (!devices.empty())
        {
            const float list_height = std::min(224.0f * s, static_cast<float>(devices.size()) * 52.0f * s);
            ImGui::BeginChild("##microphone_device_list", ImVec2(0.0f, list_height), false,
                              ImGuiWindowFlags_NoBackground);
            for (size_t index = 0; index < devices.size(); ++index)
            {
                const hmi::audio::InputDevice &device = devices[index];
                const bool is_selected = device.id == microphones.selected_id();
                ImGui::PushID(static_cast<int>(index));
                const bool clicked = ImGui::InvisibleButton("##microphone_option",
                                                            ImVec2(ImGui::GetContentRegionAvail().x, 46.0f * s));
                const bool hovered = ImGui::IsItemHovered();
                const ImVec2 row_min = ImGui::GetItemRectMin();
                const ImVec2 row_max = ImGui::GetItemRectMax();
                ImDrawList *row_draw = ImGui::GetWindowDrawList();
                if (is_selected)
                    row_draw->AddRectFilled(row_min, row_max, IM_COL32(80, 119, 176, 104), 12.0f * s);
                else if (hovered)
                    row_draw->AddRectFilled(row_min, row_max, IM_COL32(156, 187, 230, 36), 12.0f * s);

                const std::string name = ToUtf8(device.name);
                const ImU32 name_color = is_selected ? IM_COL32(244, 248, 255, 255) : IM_COL32(218, 229, 246, 255);
                const ImVec2 row_name_size = ImGui::CalcTextSize(name.c_str());
                const float name_y = row_min.y + (row_max.y - row_min.y - row_name_size.y) * 0.5f;
                row_draw->PushClipRect(ImVec2(row_min.x + 10.0f * s, row_min.y),
                                       ImVec2(row_max.x - 32.0f * s, row_max.y), true);
                row_draw->AddText(ImVec2(row_min.x + 11.0f * s, name_y), name_color, name.c_str());
                row_draw->PopClipRect();

                if (is_selected)
                {
                    const ImVec2 check(row_max.x - 17.0f * s, (row_min.y + row_max.y) * 0.5f);
                    row_draw->AddLine(ImVec2(check.x - 4.0f * s, check.y), ImVec2(check.x - 1.0f * s, check.y + 3.0f * s),
                                      IM_COL32(130, 222, 179, 255), 2.0f * s);
                    row_draw->AddLine(ImVec2(check.x - 1.0f * s, check.y + 3.0f * s), ImVec2(check.x + 5.0f * s, check.y - 4.0f * s),
                                      IM_COL32(130, 222, 179, 255), 2.0f * s);
                }
                if (clicked && microphones.select(device.id))
                    ImGui::CloseCurrentPopup();
                ImGui::PopID();
                ImGui::Dummy(ImVec2(0.0f, 5.0f * s));
            }
            ImGui::EndChild();
        }
        else
            ImGui::TextWrapped("请连接麦克风，再到设备页面刷新。");
        ImGui::EndCombo();
    }

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(5);
}

void DrawDevicesPage(hmi::app::Services &services)
{
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("设备");
    ImGui::PopFont();
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("摄像头"))
    {
        for (const auto &camera : services.cameras())
        {
            const std::string name = ToUtf8(camera.name);
            ImGui::TextWrapped("%s", name.c_str());
        }
        if (services.cameras().empty())
            ImGui::TextDisabled("未找到摄像头");
        ImGui::Spacing();
    }

    if (ImGui::Button("刷新设备"))
    {
        services.refresh_microphones();
        services.refresh_cameras();
    }
}
}
