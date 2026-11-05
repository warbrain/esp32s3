#pragma once

#include "app/mode.hpp"
#include "ui/pages/voice_page.hpp"
#include "imgui.h"

#include <vector>

struct GLFWwindow;

namespace hmi::app { class Services; }

namespace hmi::ui
{
// Per-item animation state for the navigation rail. It used to be a function
// local static array, which silently limited the shell to five pages.
struct NavigationMotion
{
    float scale = 1.0f;
    float rebound = 0.0f;
    float ripple_radius = 0.0f;
    float ripple_alpha = 0.0f;
    ImVec2 ripple_origin{};
    bool was_pressed = false;
};

struct ShellState
{
    int page = 0; // Remembered runtime page while history is open.
    std::vector<NavigationMotion> motion;
    VoicePageState voice_page;

    // Native window drag bookkeeping (kept as plain floats so this header does
    // not have to pull in windows.h).
    bool dragging = false;
    float drag_origin_x = 0.0f;
    float drag_origin_y = 0.0f;
    float window_origin_x = 0.0f;
    float window_origin_y = 0.0f;
};

// Draws the whole console window: title bar, navigation rail (including
// history) and the active page.
void DrawShell(GLFWwindow *window, ShellState &state, hmi::app::Mode &mode, hmi::app::Services &services);
}
