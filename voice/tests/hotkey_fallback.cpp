#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace fake_input
{
std::atomic<DWORD> registration_error{ERROR_ACCESS_DENIED};
std::atomic<bool> hold_down{false};
std::atomic<bool> control_down{false};
std::atomic<bool> alt_down{false};
std::atomic<int> zero_key_checks{0};
std::atomic<bool> toggle_message{false};
std::atomic<HWND> current_window{nullptr};
std::atomic<std::uintptr_t> next_window{100};
std::atomic<int> unregister_count{0};
ATOM WINAPI register_class(const WNDCLASSW *) { return 1; }
HWND WINAPI create_window(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID)
{
    const HWND value = reinterpret_cast<HWND>(next_window.fetch_add(1));
    current_window.store(value);
    return value;
}
BOOL WINAPI register_hotkey(HWND, int, UINT, UINT)
{
    const DWORD error = registration_error.load();
    if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
    return TRUE;
}
BOOL WINAPI unregister_hotkey(HWND, int) { ++unregister_count; return TRUE; }
BOOL WINAPI destroy_window(HWND) { return TRUE; }
BOOL WINAPI unregister_class(LPCWSTR, HINSTANCE) { return TRUE; }
DWORD WINAPI wait(DWORD, const HANDLE *handles, DWORD milliseconds, DWORD, DWORD)
{
    return WaitForSingleObject(handles[0], milliseconds);
}
BOOL WINAPI peek(MSG *message, HWND, UINT, UINT, UINT)
{
    if (!toggle_message.exchange(false)) return FALSE;
    message->hwnd = current_window.load();
    message->message = WM_HOTKEY;
    message->wParam = 1;
    return TRUE;
}
BOOL WINAPI translate(const MSG *) { return TRUE; }
LRESULT WINAPI dispatch(const MSG *) { return 0; }
SHORT WINAPI async_key(int key)
{
    if (key == 0) ++zero_key_checks;
    const bool down = (hold_down.load() && (key == VK_CONTROL || key == VK_SPACE)) ||
        (control_down.load() && key == VK_CONTROL) || (alt_down.load() && key == VK_MENU);
    return down ? static_cast<SHORT>(0x8000) : 0;
}
}

#define RegisterClassW fake_input::register_class
#define CreateWindowExW fake_input::create_window
#define RegisterHotKey fake_input::register_hotkey
#define UnregisterHotKey fake_input::unregister_hotkey
#define DestroyWindow fake_input::destroy_window
#define UnregisterClassW fake_input::unregister_class
#define MsgWaitForMultipleObjectsEx fake_input::wait
#define PeekMessageW fake_input::peek
#define TranslateMessage fake_input::translate
#define DispatchMessageW fake_input::dispatch
#define GetAsyncKeyState fake_input::async_key
#include "../src/input/hotkey.cpp"

int main()
{
    int failures = 0;
    auto check = [&](bool value, const char *name) {
        std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
        if (!value) ++failures;
    };
    auto eventually = [](auto condition) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!condition() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return condition();
    };
    std::atomic<int> begins{0}, ends{0}, toggles{0};
    const auto callback = [&](hmi::input::HotkeyEvent event) {
        if (event == hmi::input::HotkeyEvent::HoldBegin) ++begins;
        if (event == hmi::input::HotkeyEvent::HoldEnd) ++ends;
        if (event == hmi::input::HotkeyEvent::Toggle) ++toggles;
    };
    const hmi::input::Bindings original{"Ctrl+Alt+M", "Ctrl+Space"};
    hmi::input::Hotkeys hotkeys;
    std::string error;
    check(hotkeys.start(original, callback, &error), "toggle denial degrades to successful hold-only start");
    check(hotkeys.running(), "hold-only worker stays running");
    check(error.find("disabled") != std::string::npos && error.find("win32 5") != std::string::npos &&
          error.find("hold-to-talk remains active") != std::string::npos, "startup warning preserves failure and capability");
    fake_input::toggle_message = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(toggles == 0, "disabled toggle cannot emit callback");
    fake_input::hold_down = true;
    check(eventually([&] { return begins == 1; }), "hold begin still delivered after toggle denial");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    check(begins == 1, "held key is not repeated");
    fake_input::hold_down = false;
    check(eventually([&] { return ends == 1; }), "hold end delivered once");
    fake_input::registration_error = ERROR_HOTKEY_ALREADY_REGISTERED;
    check(!hotkeys.rebind({"Ctrl+Alt+N", "Ctrl+Space"}, &error), "rebind conflict remains strict failure");
    check(error.find("1409") != std::string::npos, "rebind exposes original conflict");
    check(hotkeys.running() && hotkeys.bindings().toggle == original.toggle, "conflicting rebind restores previous binding and hold service");
    fake_input::hold_down = true;
    check(eventually([&] { return begins == 2; }), "hold channel works after failed rebind");
    fake_input::hold_down = false;
    check(eventually([&] { return ends == 2; }), "hold release works after failed rebind");
    check(!hotkeys.rebind({"Ctrl+Space", "Ctrl+Space"}, &error) && hotkeys.running(), "invalid rebind keeps active service");
    hotkeys.stop();
    check(!hotkeys.running() && fake_input::unregister_count == 0, "denied registrations are never falsely unregistered");
    fake_input::registration_error = ERROR_SUCCESS;
    check(hotkeys.start(original, callback, &error) && error.empty(), "successful toggle registration starts without warning");
    fake_input::toggle_message = true;
    check(eventually([&] { return toggles == 1; }), "registered toggle callback preserved");
    fake_input::hold_down = true;
    check(eventually([&] { return begins == 3; }), "normal hold channel preserved");
    hotkeys.stop();
    check(ends == 3 && fake_input::unregister_count == 1, "stop releases hold and unregisters only owned toggle");
    fake_input::hold_down = false;
    const hmi::input::Bindings modifiers_only{"Ctrl+Shift+M", "Ctrl+Alt", false};
    check(hotkeys.start(modifiers_only, callback, &error) && error.empty(), "Ctrl+Alt hold starts without an ordinary key");
    check(!hotkeys.rebind({"Ctrl+Alt", "Ctrl+Space", false}, &error) && hotkeys.running(),
          "modifier-only toggle rejected without stopping the hold service");
    fake_input::control_down = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    check(begins == 3, "Ctrl alone does not start Ctrl+Alt hold");
    fake_input::alt_down = true;
    check(eventually([&] { return begins == 4; }), "Ctrl+Alt press starts hold once");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    check(begins == 4, "modifier-only hold does not repeat");
    fake_input::alt_down = false;
    check(eventually([&] { return ends == 4; }), "releasing Alt ends Ctrl+Alt hold");
    fake_input::alt_down = true;
    check(eventually([&] { return begins == 5; }), "Ctrl+Alt can begin again after release");
    fake_input::control_down = false;
    check(eventually([&] { return ends == 5; }), "releasing Ctrl ends Ctrl+Alt hold");
    fake_input::alt_down = false;
    hotkeys.stop();
    check(fake_input::zero_key_checks == 0, "modifier-only hold never polls virtual key zero");
    for (const char *invalid : {"", "+", "Ctrl+Alt+", "Ctrl+Ctrl", "Ctrl++Alt", "Ctrl+Alt+Space+"})
        check(!hotkeys.start({"Ctrl+Shift+M", invalid, false}, callback, &error), "malformed hold chord rejected");
    std::printf("%d failure(s); no real hotkeys registered, keys injected, or windows created\n", failures);
    return failures ? 1 : 0;
}
