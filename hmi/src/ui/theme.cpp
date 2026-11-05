#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace hmi::ui
{
namespace
{
constexpr float kMinUiScale = 0.75f;
constexpr float kMaxUiScale = 3.0f;

// Window / panel palette, as ImVec4 so ImGuiStyle can consume it directly.
// Draw-list colors stay next to the widgets that draw them.
constexpr ImVec4 kWindowBg{0.055f, 0.075f, 0.11f, 0.94f};
constexpr ImVec4 kChildBg{0.12f, 0.17f, 0.24f, 0.78f};
constexpr ImVec4 kBorder{0.63f, 0.76f, 0.96f, 0.24f};
constexpr ImVec4 kHeader{0.24f, 0.38f, 0.61f, 0.55f};
constexpr ImVec4 kClearColor{0.45f, 0.55f, 0.60f, 1.00f};
}

float UiScale()
{
    return std::max(kMinUiScale, ImGui::GetStyle().FontScaleDpi);
}

void LoadFonts()
{
    ImGuiIO &io = ImGui::GetIO();
    if (std::filesystem::exists("C:/Windows/Fonts/msyh.ttc"))
    {
        if (ImFont *font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 16.0f))
        {
            io.FontDefault = font;
            return;
        }
    }
    if (ImFont *ui_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 16.0f))
        io.FontDefault = ui_font;
}

void ApplyTheme(float dpi_scale)
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.WindowRounding = 18.0f;
    style.FrameRounding = 10.0f;
    style.GrabRounding = 8.0f;
    style.PopupRounding = 12.0f;
    style.ChildRounding = 18.0f;
    style.ChildBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = kWindowBg;
    style.Colors[ImGuiCol_ChildBg] = kChildBg;
    style.Colors[ImGuiCol_Border] = kBorder;
    style.Colors[ImGuiCol_Header] = kHeader;
    style.ScaleAllSizes(dpi_scale);
    style.FontScaleDpi = dpi_scale;
}

void HandleContentScale(float x_scale, float y_scale)
{
    if (!ImGui::GetCurrentContext())
        return;

    ImGuiStyle &style = ImGui::GetStyle();
    const float scale = std::clamp(std::max(x_scale, y_scale), kMinUiScale, kMaxUiScale);
    const float current = style.FontScaleDpi > 0.0f ? style.FontScaleDpi : scale;
    const float ratio = scale / current;
    if (std::abs(ratio - 1.0f) < 0.01f)
        return;

    style.ScaleAllSizes(ratio);
    style.FontScaleDpi = scale;
}

ImVec4 WindowClearColor()
{
    return kClearColor;
}

void DrawSoftGlow(ImDrawList *draw, ImVec2 center, float radius, ImU32 color)
{
    const int base_alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
    const ImU32 rgb = color & ~IM_COL32_A_MASK;
    for (int layer = 16; layer >= 1; --layer)
    {
        const float t = static_cast<float>(layer) / 16.0f;
        const int alpha = static_cast<int>(base_alpha * (1.0f - t) * .32f + 1.0f);
        draw->AddCircleFilled(center, radius * t, rgb | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT), 64);
    }
}

std::string ToUtf8(const std::wstring &value)
{
#ifdef _WIN32
    if (value.empty())
        return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), required, nullptr, nullptr);
    return result;
#else
    return std::string(value.begin(), value.end());
#endif
}
}
