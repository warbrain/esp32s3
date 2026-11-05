#include "ui/shell.hpp"

#include "app/services.hpp"
#include "ui/device_controls.hpp"
#include "ui/theme.hpp"
#include "ui/pages/voice_page.hpp"
#include "ui/pages/history_page.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

namespace hmi::ui
{
namespace
{
void DrawNavigationIcon(ImDrawList *draw, int icon, ImVec2 center, ImU32 color)
{
    const float s = UiScale();
    switch (icon)
    {
    case 1:
        draw->AddRect(ImVec2(center.x - 9.0f * s, center.y - 6.0f * s),
                      ImVec2(center.x + 9.0f * s, center.y + 7.0f * s), color, 3.0f * s, 0, 1.7f * s);
        draw->AddCircle(ImVec2(center.x, center.y + .5f * s), 3.1f * s, color, 20, 1.7f * s);
        draw->AddLine(ImVec2(center.x - 5.0f * s, center.y - 8.0f * s),
                      ImVec2(center.x + 1.0f * s, center.y - 8.0f * s), color, 1.7f * s);
        break;
    case 2:
    {
        const float heights[] = {5, 10, 14, 8, 12, 6};
        for (int i = 0; i < 6; ++i)
        {
            const float x = center.x - 10.0f * s + i * 4.0f * s;
            draw->AddLine(ImVec2(x, center.y - heights[i] * .5f * s),
                          ImVec2(x, center.y + heights[i] * .5f * s), color, 2.0f * s);
        }
        break;
    }
    case 3:
        draw->AddRect(ImVec2(center.x - 8.0f * s, center.y - 9.0f * s),
                      ImVec2(center.x + 8.0f * s, center.y + 9.0f * s), color, 2.0f * s, 0, 1.6f * s);
        for (int i = 0; i < 3; ++i)
        {
            const float y = center.y + (i * 5.0f - 5.0f) * s;
            draw->AddCircleFilled(ImVec2(center.x - 4.5f * s, y), 1.0f * s, color, 8);
            draw->AddLine(ImVec2(center.x - 1.5f * s, y),
                          ImVec2(center.x + 4.5f * s, y), color, 1.5f * s);
        }
        break;
    case 4:
        draw->AddCircle(center, 7.0f * s, color, 24, 1.7f * s);
        draw->AddCircle(center, 2.2f * s, color, 16, 1.6f * s);
        for (int i = 0; i < 8; ++i)
        {
            constexpr float kStep = 0.785398163f;
            const float a = kStep * static_cast<float>(i);
            draw->AddLine(ImVec2(center.x + std::cos(a) * 8.0f * s, center.y + std::sin(a) * 8.0f * s),
                          ImVec2(center.x + std::cos(a) * 10.0f * s, center.y + std::sin(a) * 10.0f * s),
                          color, 1.6f * s);
        }
        break;
    default:
        draw->AddCircle(center, 6.0f * s, color, 20, 1.6f * s);
        break;
    }
}

bool DrawNavigationItem(const char *label, int icon, bool selected, NavigationMotion &state, bool compact)
{
    const float s = UiScale();
    const ImVec2 size(std::max(1.0f, ImGui::GetContentRegionAvail().x), 44.0f * s);
    const ImVec2 position = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, size);

    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool activated = ImGui::IsItemActivated();
    const bool deactivated = ImGui::IsItemDeactivated();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (activated)
        state.ripple_origin = mouse;
    if (deactivated && state.was_pressed)
    {
        state.ripple_radius = 4.0f * s;
        state.ripple_alpha = .42f;
        state.rebound = .075f;
    }
    state.was_pressed = pressed;

    const float dt = std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f);
    state.rebound = std::max(0.0f, state.rebound - dt * .42f);
    const float target_scale = pressed ? .94f : 1.0f + state.rebound;
    state.scale += (target_scale - state.scale) * std::min(1.0f, dt * 24.0f);
    const float scale = state.scale;
    if (state.ripple_alpha > .01f)
    {
        state.ripple_radius += dt * 250.0f * s;
        state.ripple_alpha *= std::exp(-dt * 4.5f);
    }
    else
    {
        state.ripple_alpha = 0.0f;
        state.ripple_radius = 0.0f;
    }

    const ImVec2 center(position.x + size.x * .5f, position.y + size.y * .5f);
    const ImVec2 half(size.x * scale * .5f, size.y * scale * .5f);
    const ImVec2 visual_min(center.x - half.x, center.y - half.y);
    const ImVec2 visual_max(center.x + half.x, center.y + half.y);
    ImDrawList *draw = ImGui::GetWindowDrawList();

    const ImU32 background = selected ? IM_COL32(68, 94, 133, 172)
                                      : (hovered ? IM_COL32(61, 80, 108, 132) : IM_COL32(39, 53, 73, 82));
    draw->AddRectFilled(ImVec2(visual_min.x + s, visual_min.y + 3.0f * s),
                        ImVec2(visual_max.x + s, visual_max.y + 3.0f * s),
                        selected ? IM_COL32(0, 0, 0, 55) : IM_COL32(0, 0, 0, 14), 13.0f * s);
    draw->AddRectFilled(visual_min, visual_max, background, 13.0f * s);
    draw->AddRectFilled(ImVec2(visual_min.x + 2.0f * s, visual_min.y + 2.0f * s),
                        ImVec2(visual_max.x - 2.0f * s, visual_min.y + 14.0f * s),
                        selected ? IM_COL32(215, 232, 255, 34) : IM_COL32(215, 232, 255, hovered ? 20 : 8),
                        11.0f * s);
    draw->AddRect(visual_min, visual_max,
                  selected ? IM_COL32(174, 204, 244, 96) : IM_COL32(164, 190, 224, hovered ? 60 : 32),
                  13.0f * s, 0, selected ? 1.2f * s : 1.0f * s);
    if (selected)
    {
        draw->AddRect(ImVec2(visual_min.x - s, visual_min.y - s),
                      ImVec2(visual_max.x + s, visual_max.y + s),
                      IM_COL32(142, 184, 246, 30), 14.0f * s, 0, 1.0f * s);
        draw->AddRectFilled(ImVec2(visual_min.x + 2.0f * s, visual_min.y + 12.0f * s),
                            ImVec2(visual_min.x + 5.0f * s, visual_max.y - 12.0f * s),
                            IM_COL32(139, 190, 255, 235), 2.0f * s);
    }
    if (state.ripple_alpha > .01f)
    {
        draw->PushClipRect(position, ImVec2(position.x + size.x, position.y + size.y), true);
        draw->AddCircle(state.ripple_origin, state.ripple_radius,
                        IM_COL32(179, 216, 255, static_cast<int>(state.ripple_alpha * 170.0f)), 40, 1.5f * s);
        draw->PopClipRect();
    }

    const ImU32 tint = selected ? IM_COL32(231, 241, 255, 255)
                                : (hovered ? IM_COL32(217, 229, 246, 250) : IM_COL32(174, 190, 213, 240));
    DrawNavigationIcon(draw, icon, ImVec2(compact ? center.x : center.x - half.x + 23.0f * s * scale, center.y), tint);
    if (!compact)
        draw->AddText(ImVec2(center.x - half.x + 45.0f * s * scale,
                             center.y - ImGui::GetFontSize() * .5f), tint, label);
    if (compact && hovered)
        ImGui::SetTooltip("%s", label);
    return clicked;
}

bool DrawTrafficLight(const char *id, float x, ImU32 fill, int glyph)
{
    const float s = UiScale();
    ImGui::SetCursorPos(ImVec2(x * s, 11.0f * s));
    ImGui::InvisibleButton(id, ImVec2(18.0f * s, 18.0f * s));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const ImVec2 center((minimum.x + maximum.x) * .5f, (minimum.y + maximum.y) * .5f);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(center, 6.3f * s, fill, 24);
    draw->AddCircle(center, 6.3f * s, IM_COL32(255, 255, 255, hovered ? 100 : 42), 24, 1.0f * s);
    if (hovered)
    {
        const ImU32 mark = IM_COL32(75, 50, 49, 220);
        if (glyph == 0)
        {
            draw->AddLine(ImVec2(center.x - 2.0f * s, center.y - 2.0f * s),
                          ImVec2(center.x + 2.0f * s, center.y + 2.0f * s), mark, 1.2f * s);
            draw->AddLine(ImVec2(center.x + 2.0f * s, center.y - 2.0f * s),
                          ImVec2(center.x - 2.0f * s, center.y + 2.0f * s), mark, 1.2f * s);
        }
        else if (glyph == 1)
            draw->AddLine(ImVec2(center.x - 2.5f * s, center.y),
                          ImVec2(center.x + 2.5f * s, center.y), mark, 1.2f * s);
        else
            draw->AddRect(ImVec2(center.x - 2.2f * s, center.y - 2.2f * s),
                          ImVec2(center.x + 2.2f * s, center.y + 2.2f * s), mark, .5f * s, 0, 1.0f * s);
    }
    return clicked;
}

void DrawTitleBar(GLFWwindow *window, ShellState &state)
{
    const float s = UiScale();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 position = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth();
    draw->AddRectFilled(position, ImVec2(position.x + width, position.y + 42.0f * s),
                        IM_COL32(34, 45, 63, 210), 16.0f * s, ImDrawFlags_RoundCornersTop);

    if (DrawTrafficLight("##window_close", 14.0f, IM_COL32(255, 95, 87, 255), 0))
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    if (DrawTrafficLight("##window_minimize", 37.0f, IM_COL32(254, 188, 46, 255), 1))
        glfwIconifyWindow(window);
    if (DrawTrafficLight("##window_maximize", 60.0f, IM_COL32(40, 200, 64, 255), 2))
    {
        if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
            glfwRestoreWindow(window);
        else
            glfwMaximizeWindow(window);
    }

    draw->AddText(ImVec2(position.x + 91.0f * s, position.y + 13.0f * s),
                  IM_COL32(225, 235, 249, 245), "语音助手");

    ImGui::SetCursorPos(ImVec2(170.0f * s, 0.0f));
    ImGui::InvisibleButton("##title_drag", ImVec2(std::max(1.0f, width - 178.0f * s), 42.0f * s));
#ifdef _WIN32
    const HWND native_window = glfwGetWin32Window(window);
    if (native_window && ImGui::IsItemActivated())
    {
        POINT cursor{};
        RECT rect{};
        if (GetCursorPos(&cursor) && GetWindowRect(native_window, &rect))
        {
            state.dragging = true;
            state.drag_origin_x = static_cast<float>(cursor.x);
            state.drag_origin_y = static_cast<float>(cursor.y);
            state.window_origin_x = static_cast<float>(rect.left);
            state.window_origin_y = static_cast<float>(rect.top);
        }
    }
    if (state.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        POINT cursor{};
        if (native_window && GetCursorPos(&cursor))
            SetWindowPos(native_window, nullptr,
                         static_cast<int>(state.window_origin_x) + cursor.x - static_cast<int>(state.drag_origin_x),
                         static_cast<int>(state.window_origin_y) + cursor.y - static_cast<int>(state.drag_origin_y),
                         0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    else
        state.dragging = false;
#endif
}

void DrawSettingsPage()
{
    const float s = UiScale();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("设置");
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::SeparatorText("使用方法");
    ImGui::TextWrapped("先把光标放到要输入文字的位置。按住说话快捷键，松开后等待文字填入。");
    ImGui::Spacing();
    ImGui::TextWrapped("开启朗读后，会读出转写完成的文字；开始新的录音会停止上一段朗读。");
    ImGui::Spacing();
    ImGui::TextWrapped("文字只会填入输入框，不会自动发送。");
    ImGui::Dummy(ImVec2(0.0f, 12.0f * s));
    ImGui::SeparatorText("麦克风使用");
    ImGui::TextWrapped("在 Overview 右上角选择麦克风。");
    ImGui::Spacing();
    ImGui::TextWrapped("开始录音时麦克风才会打开，结束录音后关闭。");
}
}

void DrawShell(GLFWwindow *window, ShellState &state, hmi::app::Mode &mode, hmi::app::Services &services)
{
    const float s = UiScale();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("语音助手", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList *background = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    DrawSoftGlow(background, ImVec2(origin.x + ImGui::GetWindowWidth() * .63f, origin.y + 210.0f * s), 360.0f * s, IM_COL32(71, 125, 210, 42));
    DrawSoftGlow(background, ImVec2(origin.x + ImGui::GetWindowWidth() * .91f, origin.y + ImGui::GetWindowHeight() * .79f), 300.0f * s, IM_COL32(123, 91, 200, 32));
    DrawTitleBar(window, state);
    const float margin = 16.0f * s;
    const float gap = 12.0f * s;
    const float body_y = 58.0f * s;
    const bool compact_navigation = ImGui::GetWindowWidth() < 760.0f * s;
    const float navigation_width = (compact_navigation ? 64.0f : 136.0f) * s;
    const float body_height = std::max(1.0f, ImGui::GetWindowHeight() - body_y - margin);
    const float content_x = margin + navigation_width + gap;
    const float content_width = std::max(1.0f, ImGui::GetWindowWidth() - content_x - margin);
    ImGui::SetCursorPos(ImVec2(margin, body_y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.075f, .095f, .14f, .86f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 18.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s, 12.0f * s));
    ImGui::BeginChild("navigation", ImVec2(navigation_width, body_height), ImGuiChildFlags_Borders);

    const char *labels[] = {"Overview", "记录", "设备", "设置"};
    const int page_numbers[] = {0, -1, 2, 4};
    const int icons[] = {2, 3, 1, 4};
    const int pages = static_cast<int>(std::size(labels));
    if (static_cast<int>(state.motion.size()) < pages)
        state.motion.resize(static_cast<size_t>(pages));
    if (state.page < 0 || std::find(std::begin(page_numbers), std::end(page_numbers), state.page) == std::end(page_numbers))
        state.page = 0;

    for (int i = 0; i < pages; ++i)
    {
        ImGui::SetCursorPos(ImVec2(8.0f * s, (12.0f + i * 56.0f) * s));
        const bool history = page_numbers[i] < 0;
        const bool selected = history ? mode == hmi::app::Mode::History
                                      : mode == hmi::app::Mode::Runtime && state.page == page_numbers[i];
        if (DrawNavigationItem(labels[i], icons[i], selected,
                               state.motion[static_cast<size_t>(i)], compact_navigation))
        {
            if (!history) state.page = page_numbers[i];
            mode = history ? hmi::app::Mode::History : hmi::app::Mode::Runtime;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SetCursorPos(ImVec2(content_x, body_y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * s, 20.0f * s));
    ImGui::BeginChild("content", ImVec2(content_width, body_height), ImGuiChildFlags_Borders);
    const auto replay_speech = [&services](std::string_view text) {
        std::string error;
        (void)services.voice().request_speech(text, &error);
    };
    if (mode == hmi::app::Mode::History)
        DrawHistoryPage(services.voice_snapshot(), replay_speech);
    else if (state.page == 0)
        DrawVoicePage(services.voice_snapshot(), state.voice_page, [&services] {
            std::string error;
            (void)services.voice().request_speech("你好，语音操控已就绪。", &error);
        }, [&services](bool enabled) { return services.set_tts_enabled(enabled); },
        replay_speech, [&services] { DrawMicrophonePicker(services.microphones()); });
    else if (state.page == 2)
        DrawDevicesPage(services);
    else
        DrawSettingsPage();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleVar(2);
}
}
