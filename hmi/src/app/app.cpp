#include "app/app.hpp"
#include "base/frame_schedule.hpp"
#include "app/capture_presentation.hpp"

#include "imgui.h"
#include <GLFW/glfw3.h>

namespace hmi::app
{
App::App()
{
    services_.prepare();
}

void App::start()
{
    services_.start();
}

void App::frame(GLFWwindow *window)
{
    ImGuiIO &io = ImGui::GetIO();
    if (!io.WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E))
        mode_ = (mode_ == Mode::Runtime) ? Mode::History : Mode::Runtime;

    services_.poll(io.DeltaTime);
    if (io.UserData)
    {
        const auto voice = services_.voice_snapshot();
        auto &activity = *static_cast<base::FrameActivity *>(io.UserData);
        activity.animation_active = voice.capture_active || voice.state == VoiceState::Starting ||
            voice.state == VoiceState::Listening || voice.refining;
        activity.capture = detail::make_capture_presentation(voice);
    }
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) return;
    ui::DrawShell(window, shell_, mode_, services_);
}

void App::shutdown()
{
    services_.shutdown();
}
}
