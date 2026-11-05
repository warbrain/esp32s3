#pragma once

#include <functional>
#include <memory>
#include <string>

namespace hmi::input
{
enum class HotkeyEvent
{
    Toggle,    // registered hotkey pressed once: start/stop listening
    HoldBegin, // push-to-talk chord went down
    HoldEnd    // push-to-talk chord released
};

struct Bindings
{
    std::string toggle = "Ctrl+Shift+M";
    std::string hold = "Ctrl+Space";
    bool use_config_file = true; // Compatibility JSON; application YAML passes false.
};

// Global hotkeys. The toggle binding uses RegisterHotKey on a dedicated
// message-only window; push-to-talk polls GetAsyncKeyState so it also works
// while another application owns the foreground window.
class Hotkeys
{
public:
    using Handler = std::function<void(HotkeyEvent)>;

    Hotkeys();
    ~Hotkeys();
    Hotkeys(const Hotkeys &) = delete;
    Hotkeys &operator=(const Hotkeys &) = delete;

    bool start(const Bindings &bindings, Handler handler, std::string *error);
    void stop();
    bool running() const noexcept;

    // Returns false when a chord is already claimed by another application.
    bool rebind(const Bindings &bindings, std::string *error);
    const Bindings &bindings() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
