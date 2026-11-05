#pragma once

#include <functional>
#include <memory>

struct GLFWwindow;
namespace hmi::base { class Logger; }

namespace hmi::platform
{
// Owns the GLFW window, the Vulkan device and the Dear ImGui backends, and
// drives the frame loop. main.cpp should not need to know any of that.
class Desktop
{
public:
    using FrameCallback = std::function<void(GLFWwindow *window)>;

    explicit Desktop(base::Logger *logger = nullptr);
    ~Desktop();

    Desktop(const Desktop &) = delete;
    Desktop &operator=(const Desktop &) = delete;

    bool available() const noexcept;

    // Blocks until the window closes, calling `frame` once per frame.
    void run(const FrameCallback &frame, base::Logger *logger = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool initialized_ = false;
};
}
