#pragma once

#include "app/mode.hpp"
#include "app/services.hpp"
#include "ui/shell.hpp"

struct GLFWwindow;

namespace hmi::app
{
// Application coordinator. Desktop owns the window, Vulkan device and ImGui
// context; App owns services, page selection and per-frame UI coordination.
class App
{
public:
    App();

    void start();
    void frame(GLFWwindow *window);
    void shutdown();
    base::Logger *diagnostic_logger() noexcept { return services_.diagnostic_logger(); }

private:
    Mode mode_ = Mode::Runtime;
    Services services_;
    ui::ShellState shell_;
};
}
