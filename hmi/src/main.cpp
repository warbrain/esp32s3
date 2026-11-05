#include "app/app.hpp"
#include "platform/desktop.hpp"

#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

// Composition root. The GLFW window, the Vulkan device and the ImGui backends
// live in platform::Desktop; the console itself lives in app::App. If this file
// keeps growing, something belongs in one of those instead.
static int run()
{
    hmi::app::App app;
    auto *logger = app.diagnostic_logger();
    int result = 0;
    {
        hmi::platform::Desktop desktop(logger);
        if (!desktop.available())
        {
            std::fprintf(stderr, "HMI: failed to create the desktop or Vulkan context\n");
            result = 1;
        }
        else
        {
            app.start();
            desktop.run([&app](GLFWwindow *window)
                        { app.frame(window); }, logger);
        }
    }
    app.shutdown();
    return result;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    // Configure loader error reporting before any worker can load plugins.
    // Missing SDK dependencies are reported by the voice status instead of
    // blocking the application behind a native loader dialog.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    return run();
}
#else
int main(int, char **) { return run(); }
#endif
