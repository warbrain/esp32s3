#pragma once

#include "imgui.h"

#include <string>

namespace hmi::ui
{
// Visual language shared by the shell and its pages.
float UiScale();

// Loads the UI font. Must run before the renderer backend builds its atlas.
void LoadFonts();

// Applies the dark glass style at the given DPI scale.
void ApplyTheme(float dpi_scale);

// Re-scales the style when the window moves to a monitor with another DPI.
void HandleContentScale(float x_scale, float y_scale);

ImVec4 WindowClearColor();

void DrawSoftGlow(ImDrawList *draw, ImVec2 center, float radius, ImU32 color);

std::string ToUtf8(const std::wstring &value);
}
