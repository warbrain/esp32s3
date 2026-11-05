#include "base/capture_presentation.hpp"
#include "platform/capture_overlay.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string_view>
#include <vector>

namespace
{
struct WindowOwner
{
    HWND window = nullptr;
    ~WindowOwner() { if (window) DestroyWindow(window); }
    WindowOwner(const WindowOwner &) = delete;
    WindowOwner &operator=(const WindowOwner &) = delete;
    WindowOwner() = default;
};

struct Probe
{
    int failures = 0;

    void check(bool condition, const char *description)
    {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
        if (!condition) ++failures;
    }
};

BOOL CALLBACK collect_overlay(HWND window, LPARAM context)
{
    wchar_t name[128]{};
    if (GetClassNameW(window, name, static_cast<int>(std::size(name))) == 0) return TRUE;
    constexpr wchar_t prefix[] = L"HmiCaptureOverlay_";
    if (std::wcsncmp(name, prefix, std::size(prefix) - 1) == 0)
        reinterpret_cast<std::vector<HWND> *>(context)->push_back(window);
    return TRUE;
}

std::vector<HWND> overlay_windows()
{
    std::vector<HWND> windows;
    EnumThreadWindows(GetCurrentThreadId(), collect_overlay, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

void pump_messages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool interactive_station()
{
    USEROBJECTFLAGS flags{};
    DWORD needed = 0;
    return GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS,
        &flags, static_cast<DWORD>(sizeof(flags)), &needed) && (flags.dwFlags & WSF_VISIBLE) != 0;
}

hmi::base::CapturePresentation recording(std::uint64_t request, double seconds)
{
    hmi::base::CapturePresentation result;
    result.phase = hmi::base::CapturePhase::Recording;
    result.request_id = request;
    result.captured_frames = static_cast<std::size_t>(seconds * 16000.0);
    result.limit_seconds = 30;
    for (std::size_t i = 0; i < result.levels.size(); ++i)
    {
        const double wave = 0.5 + 0.5 * std::sin(static_cast<double>(i) * 0.55 - seconds * 4.5);
        result.levels[i] = static_cast<float>(0.002 + wave * 0.045);
    }
    result.live_level = result.levels.back();
    if (seconds >= 1.0)
    {
        const char *caption = seconds < 3.0 ? "咱们周五晚上在太原南过夜" :
            "咱们周五晚上在太原南过夜，然后第二天去临汾租车，时间比较紧张，因为没有直达的火车。";
        std::snprintf(result.transcript.data(), result.transcript.size(), "%s", caption);
    }
    return result;
}

void check_input_transparency(Probe &probe, HWND overlay, HWND host)
{
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(overlay, GWL_STYLE));
    const auto extended = static_cast<DWORD>(GetWindowLongPtrW(overlay, GWL_EXSTYLE));
    constexpr DWORD required = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_TRANSPARENT | WS_EX_LAYERED;
    probe.check((style & WS_POPUP) != 0 && (style & WS_CHILD) == 0, "overlay is an independent popup");
    probe.check((extended & required) == required && (extended & WS_EX_APPWINDOW) == 0,
        "overlay is topmost, nonactivating, click-through, and absent from the taskbar");
    const auto activation = SendMessageW(overlay, WM_MOUSEACTIVATE,
        reinterpret_cast<WPARAM>(host), MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
    probe.check(activation == MA_NOACTIVATE, "mouse activation request is refused");
    RECT bounds{};
    const bool has_bounds = GetWindowRect(overlay, &bounds) != FALSE;
    const int x = bounds.left + (bounds.right - bounds.left) / 2;
    const int y = bounds.top + (bounds.bottom - bounds.top) / 2;
    const LPARAM point = static_cast<LPARAM>(MAKELONG(static_cast<WORD>(x), static_cast<WORD>(y)));
    probe.check(has_bounds && SendMessageW(overlay, WM_NCHITTEST, 0, point) == HTTRANSPARENT,
        "hit testing passes through the overlay");
    MONITORINFO monitor{sizeof(MONITORINFO)};
    const auto nearest = MonitorFromWindow(overlay, MONITOR_DEFAULTTONEAREST);
    const bool has_monitor = GetMonitorInfoW(nearest, &monitor) != FALSE;
    probe.check(has_bounds && has_monitor && bounds.left >= monitor.rcWork.left &&
        bounds.top >= monitor.rcWork.top && bounds.right <= monitor.rcWork.right &&
        bounds.bottom <= monitor.rcWork.bottom, "overlay stays inside the monitor work area");
    const int center_error = std::abs((bounds.left + bounds.right) - (monitor.rcWork.left + monitor.rcWork.right));
    const int bottom_margin = monitor.rcWork.bottom - bounds.bottom;
    probe.check(has_bounds && has_monitor && center_error <= 4 && bottom_margin >= 0 &&
        bottom_margin <= std::max(64, static_cast<int>((bounds.bottom - bounds.top) * 2)),
        "overlay is centered near the bottom of the work area");
}

int run_checks()
{
    if (!interactive_station())
    {
        std::printf("SKIP interactive window station unavailable; error=%lu\n", GetLastError());
        return 77;
    }

    WindowOwner host;
    host.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Capture overlay test host", WS_OVERLAPPEDWINDOW, 0, 0, 240, 120,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!host.window)
    {
        const DWORD error = GetLastError();
        const bool inaccessible = error == ERROR_ACCESS_DENIED || error == ERROR_REQUIRES_INTERACTIVE_WINDOWSTATION;
        std::printf("%s cannot create test host; error=%lu\n", inaccessible ? "SKIP" : "FAIL", error);
        return inaccessible ? 77 : 1;
    }

    Probe probe;
    const HWND foreground = GetForegroundWindow();
    ShowWindow(host.window, SW_SHOWMINNOACTIVE);
    pump_messages();
    probe.check(IsIconic(host.window) != FALSE, "simulated main window is minimized");
    probe.check(GetForegroundWindow() == foreground, "test host does not change the foreground window");

    HWND created = nullptr;
    const DWORD resources_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    {
        hmi::platform::CaptureOverlay overlay;
        hmi::base::CapturePresentation presentation;
        overlay.update(presentation);
        probe.check(!overlay.visible() && overlay_windows().empty(), "initial hidden state creates no popup");

        presentation = recording(100, 1.5);
        overlay.update(presentation);
        pump_messages();
        const auto windows = overlay_windows();
        probe.check(overlay.last_error() == 0, "recording presentation has no native error");
        probe.check(overlay.visible() && windows.size() == 1 && IsWindowVisible(windows.front()),
            "recording popup remains visible with the main window minimized");
        probe.check(GetForegroundWindow() == foreground, "recording does not steal the foreground window");
        if (windows.size() == 1)
        {
            created = windows.front();
            check_input_transparency(probe, created, host.window);
        }

        const DWORD resources_warm = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 160; ++i)
        {
            presentation = recording(100, 1.5 + static_cast<double>(i) / 160.0);
            overlay.update(presentation);
            pump_messages();
            Sleep(2);
        }
        const DWORD resources_repeated = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        probe.check(resources_warm > 0 && resources_repeated <= resources_warm + 2,
            "repeated waveform updates do not accumulate GDI objects");
        probe.check(GetForegroundWindow() == foreground, "repeated waveform updates preserve the foreground window");

        presentation.phase = hmi::base::CapturePhase::Processing;
        overlay.update(presentation);
        pump_messages();
        probe.check(overlay.visible(), "processing remains visible");
        presentation.phase = hmi::base::CapturePhase::AwaitingRelease;
        overlay.update(presentation);
        pump_messages();
        probe.check(overlay.visible(), "waiting for release remains visible");

        presentation.phase = hmi::base::CapturePhase::Hidden;
        overlay.update(presentation);
        probe.check(!overlay.visible() && (!created || !IsWindowVisible(created)), "completed request hides the popup");
        presentation = recording(101, 0.0);
        presentation.levels.fill(0.0f);
        presentation.live_level = 0.0f;
        overlay.update(presentation);
        pump_messages();
        probe.check(overlay.visible(), "next request shows the popup again from zero captured frames");
        presentation.phase = hmi::base::CapturePhase::Preparing;
        overlay.update(presentation);
        probe.check(overlay.visible(), "preparing remains visible");
        overlay.hide();
        probe.check(!overlay.visible() && (!created || !IsWindowVisible(created)), "explicit hide removes the popup");
        overlay.update(recording(102, 0.25));
        pump_messages();
        probe.check(overlay.visible() && overlay.last_error() == 0, "same overlay can be reused after explicit hide");
        probe.check(GetForegroundWindow() == foreground, "phase transitions and redisplay preserve the foreground window");
    }
    pump_messages();
    probe.check(overlay_windows().empty() && (!created || !IsWindow(created)), "destruction releases the native popup");
    const DWORD resources_after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    probe.check(resources_after <= resources_before + 2, "destruction releases overlay GDI resources");
    std::printf("capture_overlay_integration failures=%d gdi_before=%lu gdi_after=%lu\n",
        probe.failures, resources_before, resources_after);
    return probe.failures == 0 ? 0 : 1;
}

int preview(int duration_seconds)
{
    if (!interactive_station())
    {
        std::puts("SKIP interactive window station unavailable");
        return 77;
    }
    const HWND foreground = GetForegroundWindow();
    hmi::platform::CaptureOverlay overlay;
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    std::printf("Synthetic capture preview: %d seconds, no microphone, no text input.\n", duration_seconds);
    std::fflush(stdout);
    while (true)
    {
        const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        if (elapsed >= static_cast<double>(duration_seconds)) break;
        auto presentation = recording(200, std::max(0.0, elapsed - 1.0));
        if (elapsed < 1.0) presentation.phase = hmi::base::CapturePhase::Preparing;
        else if (elapsed >= static_cast<double>(duration_seconds) - 1.0)
            presentation.phase = hmi::base::CapturePhase::AwaitingRelease;
        else if (elapsed >= static_cast<double>(duration_seconds) - 4.0)
            presentation.phase = hmi::base::CapturePhase::Processing;
        overlay.update(presentation);
        if (overlay.last_error() != 0)
        {
            std::printf("FAIL preview error=%lu\n", static_cast<unsigned long>(overlay.last_error()));
            return 1;
        }
        pump_messages();
        Sleep(16);
    }
    overlay.hide();
    std::printf("Synthetic preview finished; foreground_unchanged=%s\n",
        GetForegroundWindow() == foreground ? "true" : "false");
    return 0;
}
}

int main(int argc, char **argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc == 1) return run_checks();
    if (argc == 2 && std::string_view(argv[1]) == "--preview") return preview(30);
    if (argc == 3 && std::string_view(argv[1]) == "--preview-seconds")
    {
        const std::string_view value(argv[2]);
        int seconds = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && seconds >= 5 && seconds <= 60)
            return preview(seconds);
    }
    std::fputs("Usage: capture_overlay_integration [--preview | --preview-seconds 5..60]\n", stderr);
    return 2;
}
