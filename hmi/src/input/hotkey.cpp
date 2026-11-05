#include "input/hotkey.hpp"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include "base/project_paths.hpp"
#endif

namespace hmi::input
{
namespace
{
class ConfigReader
{
public:
    explicit ConfigReader(std::string_view text) : text_(text) {}

    bool read(Bindings &bindings)
    {
        if (!take('{')) return false;
        bool toggle_seen = false;
        bool hold_seen = false;
        if (!take('}'))
        {
            do
            {
                std::string key;
                std::string value;
                if (!string(key) || !take(':') || !string(value)) return false;
                if (key == "toggle" && !toggle_seen)
                {
                    bindings.toggle = std::move(value);
                    toggle_seen = true;
                }
                else if (key == "hold" && !hold_seen)
                {
                    bindings.hold = std::move(value);
                    hold_seen = true;
                }
                else return false;
            } while (take(','));
            if (!take('}')) return false;
        }
        whitespace();
        return cursor_ == text_.size();
    }

private:
    void whitespace()
    {
        while (cursor_ < text_.size() && (text_[cursor_] == ' ' || text_[cursor_] == '\n' ||
               text_[cursor_] == '\r' || text_[cursor_] == '\t')) ++cursor_;
    }

    bool take(char wanted)
    {
        whitespace();
        if (cursor_ == text_.size() || text_[cursor_] != wanted) return false;
        ++cursor_;
        return true;
    }

    bool string(std::string &out)
    {
        if (!take('"')) return false;
        while (cursor_ < text_.size())
        {
            char value = text_[cursor_++];
            if (value == '"') return true;
            if (static_cast<unsigned char>(value) < 0x20) return false;
            if (value == '\\')
            {
                if (cursor_ == text_.size()) return false;
                value = text_[cursor_++];
                if (value != '"' && value != '\\' && value != '/') return false;
            }
            out += value;
        }
        return false;
    }

    std::string_view text_;
    std::size_t cursor_ = 0;
};

bool load_config(Bindings &bindings, std::string *error)
{
    if (!bindings.use_config_file) return true;
    // Explicit caller bindings take precedence over the optional default file.
    const Bindings defaults;
    if (bindings.toggle != defaults.toggle || bindings.hold != defaults.hold) return true;
#ifdef _WIN32
    const auto directory = base::process_directory();
    std::filesystem::path config;
    if (!base::resolve_project_path(directory / "config/hotkeys.json", directory, config, error)) return false;
#else
    std::filesystem::path config = std::filesystem::path("config") / "hotkeys.json";
#endif
    std::error_code code;
    if (!std::filesystem::exists(config, code)) return true;
    std::ifstream file(config, std::ios::binary);
    if (!file)
    {
        if (error) *error = "cannot open config/hotkeys.json";
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (!ConfigReader(text).read(bindings))
    {
        if (error) *error = "invalid config/hotkeys.json: expected toggle/hold string bindings";
        return false;
    }
    return true;
}

#ifdef _WIN32
struct Chord
{
    UINT modifiers = 0;
    UINT key = 0;
};

struct Startup
{
    bool ok = false;
    std::string detail;
};

Startup registration_result(bool registered, bool allow_hold_fallback, std::string failure)
{
    if (registered) return {true, {}};
    if (!allow_hold_fallback) return {false, std::move(failure)};
    return {true, "Toggle hotkey disabled: " + failure + "; hold-to-talk remains active"};
}

std::string token(std::string_view value)
{
    std::string normalized;
    for (const unsigned char c : value)
        if (c != ' ' && c != '\t') normalized += static_cast<char>(std::toupper(c));
    return normalized;
}

UINT virtual_key(const std::string &name)
{
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') ||
                             (name[0] >= '0' && name[0] <= '9')))
        return static_cast<UINT>(name[0]);
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F')
    {
        unsigned number = 0;
        for (std::size_t index = 1; index < name.size(); ++index)
        {
            if (name[index] < '0' || name[index] > '9') return 0;
            number = number * 10 + static_cast<unsigned>(name[index] - '0');
        }
        if (number >= 1 && number <= 24) return VK_F1 + number - 1;
    }
    struct NamedKey { const char *name; UINT value; };
    constexpr NamedKey keys[]{
        {"SPACE", VK_SPACE}, {"RETURN", VK_RETURN}, {"ENTER", VK_RETURN},
        {"TAB", VK_TAB}, {"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE},
        {"BACKSPACE", VK_BACK}, {"DELETE", VK_DELETE}, {"INSERT", VK_INSERT},
        {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR},
        {"PAGEDOWN", VK_NEXT}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"UP", VK_UP}, {"DOWN", VK_DOWN}
    };
    for (const NamedKey &key : keys)
        if (name == key.name) return key.value;
    return 0;
}

bool parse_chord(std::string_view text, Chord &chord, bool allow_modifiers_only = false)
{
    std::size_t start = 0;
    while (start < text.size())
    {
        const std::size_t end = text.find('+', start);
        const std::string name = token(text.substr(start, end == std::string_view::npos ? end : end - start));
        UINT modifier = 0;
        if (name == "CTRL" || name == "CONTROL") modifier = MOD_CONTROL;
        else if (name == "SHIFT") modifier = MOD_SHIFT;
        else if (name == "ALT") modifier = MOD_ALT;
        else if (name == "WIN" || name == "WINDOWS") modifier = MOD_WIN;
        if (modifier)
        {
            if (chord.key || (chord.modifiers & modifier)) return false;
            chord.modifiers |= modifier;
        }
        else
        {
            if (chord.key) return false;
            chord.key = virtual_key(name);
            if (!chord.key) return false;
        }
        if (end == std::string_view::npos)
            return chord.key != 0 || (allow_modifiers_only && chord.modifiers != 0);
        start = end + 1;
    }
    return false;
}

bool is_down(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

bool chord_down(const Chord &chord)
{
    UINT modifiers = 0;
    if (is_down(VK_CONTROL)) modifiers |= MOD_CONTROL;
    if (is_down(VK_SHIFT)) modifiers |= MOD_SHIFT;
    if (is_down(VK_MENU)) modifiers |= MOD_ALT;
    if (is_down(VK_LWIN) || is_down(VK_RWIN)) modifiers |= MOD_WIN;
    return modifiers == chord.modifiers && (chord.key == 0 || is_down(static_cast<int>(chord.key)));
}
#endif
}

struct Hotkeys::Impl
{
    Bindings bindings;
    Handler handler;
    std::atomic<bool> active{false};
    std::thread worker;
#ifdef _WIN32
    HANDLE stop_event = nullptr;

    void notify(HotkeyEvent event) const noexcept
    {
        try { handler(event); }
        catch (...) { /* A client callback must not unwind through a Windows message loop. */ }
    }

    void run(Chord toggle, Chord hold, bool allow_hold_fallback, std::promise<Startup> initialized)
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const std::wstring class_name = L"HMI.VoiceHotkeys." +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(this));
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = instance;
        window_class.lpszClassName = class_name.c_str();
        if (!RegisterClassW(&window_class))
        {
            initialized.set_value({false, "RegisterClassW failed (win32 " + std::to_string(GetLastError()) + ")"});
            return;
        }
        const HWND window = CreateWindowExW(0, class_name.c_str(), L"HMI hotkeys", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        if (!window)
        {
            initialized.set_value({false, "CreateWindowExW failed (win32 " + std::to_string(GetLastError()) + ")"});
            UnregisterClassW(class_name.c_str(), instance);
            return;
        }
        const bool toggle_registered = RegisterHotKey(window, 1, toggle.modifiers | MOD_NOREPEAT, toggle.key) != FALSE;
        const std::string failure = toggle_registered ? std::string{} :
            "RegisterHotKey conflict or unavailable (win32 " + std::to_string(GetLastError()) + ")";
        Startup startup = registration_result(toggle_registered, allow_hold_fallback, failure);
        if (!startup.ok)
        {
            initialized.set_value(std::move(startup));
            DestroyWindow(window);
            UnregisterClassW(class_name.c_str(), instance);
            return;
        }
        active = true;
        initialized.set_value(std::move(startup));
        bool held = false;
        bool quitting = false;
        while (!quitting)
        {
            const DWORD wait = MsgWaitForMultipleObjectsEx(1, &stop_event, 10,
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT) { quitting = true; break; }
                if (toggle_registered && message.message == WM_HOTKEY && message.hwnd == window && message.wParam == 1)
                    notify(HotkeyEvent::Toggle);
                else
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            if (quitting) break;
            const bool down = chord_down(hold);
            if (down != held)
            {
                held = down;
                notify(held ? HotkeyEvent::HoldBegin : HotkeyEvent::HoldEnd);
            }
        }
        if (held) notify(HotkeyEvent::HoldEnd);
        if (toggle_registered) UnregisterHotKey(window, 1);
        DestroyWindow(window);
        UnregisterClassW(class_name.c_str(), instance);
        active = false;
    }

    bool launch(const Bindings &wanted, Handler callback, bool allow_hold_fallback, std::string *error)
    {
        Chord toggle;
        Chord hold;
        if (!parse_chord(wanted.toggle, toggle) || !parse_chord(wanted.hold, hold, true))
        {
            if (error) *error = "invalid hotkey chord (toggle requires a key; hold also accepts modifiers only)";
            return false;
        }
        if (toggle.modifiers == hold.modifiers && toggle.key == hold.key)
        {
            if (error) *error = "toggle and hold must use different chords";
            return false;
        }
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event)
        {
            if (error) *error = "CreateEventW failed (win32 " + std::to_string(GetLastError()) + ")";
            return false;
        }
        handler = std::move(callback);
        std::promise<Startup> initialized;
        auto ready = initialized.get_future();
        try
        {
            worker = std::thread(&Impl::run, this, toggle, hold, allow_hold_fallback, std::move(initialized));
            const Startup startup = ready.get();
            if (error) *error = startup.detail;
            if (startup.ok) { bindings = wanted; return true; }
        }
        catch (const std::exception &exception)
        {
            if (error) *error = exception.what();
        }
        finish();
        return false;
    }

    void finish()
    {
        if (stop_event) SetEvent(stop_event);
        if (worker.joinable()) worker.join();
        if (stop_event) CloseHandle(stop_event);
        stop_event = nullptr;
        active = false;
    }
#endif
};

Hotkeys::Hotkeys() : impl_(std::make_unique<Impl>()) {}
Hotkeys::~Hotkeys() { stop(); }

bool Hotkeys::start(const Bindings &bindings, Handler handler, std::string *error)
{
    stop();
    if (error) error->clear();
    if (!handler)
    {
        if (error) *error = "hotkeys require an event handler";
        return false;
    }
    Bindings configured = bindings;
    if (!load_config(configured, error)) return false;
#ifdef _WIN32
    // RegisterHotKey and GetAsyncKeyState are independent channels. A toggle
    // conflict must not disable hold-to-talk. A nonempty error on success is
    // the warning that the caller must display instead of advertising toggle.
    return impl_->launch(configured, std::move(handler), true, error);
#else
    if (error) *error = "global hotkeys require Windows";
    return false;
#endif
}

void Hotkeys::stop()
{
#ifdef _WIN32
    impl_->finish();
#endif
}

bool Hotkeys::running() const noexcept { return impl_->active.load(); }

bool Hotkeys::rebind(const Bindings &bindings, std::string *error)
{
    if (error) error->clear();
#ifdef _WIN32
    Chord toggle;
    Chord hold;
    if (!parse_chord(bindings.toggle, toggle) || !parse_chord(bindings.hold, hold, true))
    {
        if (error) *error = "invalid hotkey chord";
        return false;
    }
    if (toggle.modifiers == hold.modifiers && toggle.key == hold.key)
    {
        if (error) *error = "toggle and hold must use different chords";
        return false;
    }
    if (!running())
    {
        if (error) *error = "hotkeys are not running";
        return false;
    }
    const Bindings previous = impl_->bindings;
    const Handler callback = impl_->handler;
    stop();
    // Rebind is strict: a conflicting requested toggle is a failed rebind,
    // even when the original service was already operating in hold-only mode.
    if (impl_->launch(bindings, callback, false, error)) return true;
    std::string restore_error;
    const bool restored = impl_->launch(previous, callback, true, &restore_error);
    if (error)
    {
        if (!restored) *error += "; restoring previous bindings failed: " + restore_error;
        else if (!restore_error.empty()) *error += "; previous bindings restored with warning: " + restore_error;
    }
#else
    (void)bindings;
    if (error) *error = "global hotkeys require Windows";
#endif
    return false;
}

const Bindings &Hotkeys::bindings() const noexcept
{
    return impl_->bindings;
}
}
