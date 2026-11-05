#include "platform/capture_overlay.hpp"
#include "base/capture_presentation.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <string>

namespace hmi::platform
{
namespace
{
using Clock = std::chrono::steady_clock;

COLORREF blend(COLORREF background, COLORREF foreground, float amount)
{
    const auto channel = [amount](BYTE from, BYTE to) {
        return static_cast<BYTE>(static_cast<float>(from) + (static_cast<float>(to) - from) * amount);
    };
    return RGB(channel(GetRValue(background), GetRValue(foreground)),
        channel(GetGValue(background), GetGValue(foreground)), channel(GetBValue(background), GetBValue(foreground)));
}

float level_amount(float level)
{
    const float rms = std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
    return std::clamp((20.0f * std::log10(std::max(rms, 0.000001f)) + 72.0f) / 60.0f, 0.0f, 1.0f);
}

class ScreenDc
{
public:
    ScreenDc() : value(GetDC(nullptr)) {}
    ~ScreenDc() { if (value) ReleaseDC(nullptr, value); }
    HDC value = nullptr;
};

class DibBuffer
{
public:
    ~DibBuffer()
    {
        if (dc_ && previous_) SelectObject(dc_, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
    }
    bool resize(HDC screen, int width, int height)
    {
        if (width == width_ && height == height_ && bitmap_) return true;
        if (!dc_) dc_ = CreateCompatibleDC(screen);
        if (!dc_) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *pixels = nullptr;
        const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !pixels) { if (bitmap) DeleteObject(bitmap); return false; }
        const HGDIOBJ previous = SelectObject(dc_, bitmap);
        if (!previous || previous == HGDI_ERROR) { DeleteObject(bitmap); return false; }
        if (!previous_) previous_ = previous;
        if (bitmap_) DeleteObject(bitmap_);
        bitmap_ = bitmap;
        pixels_ = pixels;
        width_ = width;
        height_ = height;
        return true;
    }
    void clear()
    {
        GdiFlush();
        std::memset(pixels_, 0, static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4);
    }
    HDC dc() const noexcept { return dc_; }
private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void *pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

class SelectedObject
{
public:
    SelectedObject(HDC dc, HGDIOBJ object) : dc_(dc), previous_(SelectObject(dc, object)) {}
    ~SelectedObject() { if (previous_ && previous_ != HGDI_ERROR) SelectObject(dc_, previous_); }
private:
    HDC dc_;
    HGDIOBJ previous_;
};

void rounded(HDC dc, int x, int y, int width, int height, int radius, COLORREF fill, COLORREF border)
{
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, border);
    RoundRect(dc, x, y, x + width, y + height, radius * 2, radius * 2);
}

void circle(HDC dc, int x, int y, int radius, COLORREF color)
{
    SetDCBrushColor(dc, color);
    SetDCPenColor(dc, color);
    Ellipse(dc, x - radius, y - radius, x + radius + 1, y + radius + 1);
}

int text_width(HDC dc, const wchar_t *text)
{
    SIZE size{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(std::wcslen(text)), &size);
    return static_cast<int>(size.cx);
}

void text(HDC dc, const wchar_t *value, int x, int y, int width, int height, COLORREF color, UINT align = DT_LEFT)
{
    SetTextColor(dc, color);
    RECT rectangle{x, y, x + width, y + height};
    DrawTextW(dc, value, -1, &rectangle, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void microphone(HDC dc, int x, int y, float scale, COLORREF color)
{
    const auto px = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
    rounded(dc, x - px(3), y - px(9), px(6), px(12), px(3), color, color);
    const HPEN outline = CreatePen(PS_SOLID, std::max(1, px(1.5f)), color);
    if (outline)
    {
        {
            const SelectedObject selected(dc, outline);
            Arc(dc, x - px(6), y - px(4), x + px(6), y + px(7),
                x - px(6), y, x + px(6), y);
            MoveToEx(dc, x, y + px(7), nullptr);
            LineTo(dc, x, y + px(11));
            MoveToEx(dc, x - px(4), y + px(11), nullptr);
            LineTo(dc, x + px(4), y + px(11));
        }
        DeleteObject(outline);
    }
}

void transcript_text(HDC dc, const base::CapturePresentation &value, int x, int y,
                     int width, int height, COLORREF color)
{
    std::array<wchar_t, 1024> wide{};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.transcript.data(), -1,
        wide.data(), static_cast<int>(wide.size()));
    if (count <= 1)
    {
        text(dc, L"说话后，文字会显示在这里", x, y, width, height, color);
        return;
    }
    const int length = count - 1;
    std::array<int, 1024> advances{};
    SIZE extent{};
    int fitting = 0;
    if (!GetTextExtentExPointW(dc, wide.data(), length, std::numeric_limits<int>::max(), &fitting, advances.data(), &extent) ||
        fitting != length ||
        extent.cx <= width)
    {
        text(dc, wide.data(), x, y, width, height, color);
        return;
    }
    const int ellipsis_width = text_width(dc, L"…");
    const int available = std::max(0, width - ellipsis_width);
    int offset = 0;
    while (offset < length && extent.cx - (offset > 0 ? advances[static_cast<std::size_t>(offset - 1)] : 0) > available)
        ++offset;
    // Never start at the low half of a UTF-16 surrogate pair.
    if (offset < length && wide[static_cast<std::size_t>(offset)] >= 0xDC00 &&
        wide[static_cast<std::size_t>(offset)] <= 0xDFFF) ++offset;
    text(dc, L"…", x, y, std::min(width, ellipsis_width), height, color);
    if (available > 0)
        text(dc, wide.data() + offset, x + ellipsis_width, y, available, height, color);
}
}

struct CaptureOverlay::Impl
{
    using DpiForMonitor = HRESULT (WINAPI *)(HMONITOR, int, UINT *, UINT *);
    HWND window = nullptr;
    HINSTANCE instance = nullptr;
    ATOM atom = 0;
    std::wstring class_name;
    DibBuffer buffer;
    HFONT font = nullptr;
    HFONT clock_font = nullptr;
    UINT font_dpi = 0;
    HMODULE dpi_module = nullptr;
    DpiForMonitor dpi_for_monitor = nullptr;
    bool dpi_attempted = false;
    bool shown = false;
    DWORD error = ERROR_SUCCESS;
    std::uint64_t request_id = 0;
    base::CapturePhase phase = base::CapturePhase::Hidden;
    std::array<float, 64> levels{};
    Clock::time_point updated{};
    Clock::time_point painted{};
    Clock::time_point retry_after{};
    POINT location{};
    SIZE dimensions{};
    UINT previous_dpi = 0;

    ~Impl()
    {
        hide_window();
        if (window) DestroyWindow(window);
        if (atom) UnregisterClassW(class_name.c_str(), instance);
        if (font) DeleteObject(font);
        if (clock_font) DeleteObject(clock_font);
        if (dpi_module) FreeLibrary(dpi_module);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window = hwnd;
        }
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }
        if (self && message == WM_CLOSE) { self->hide_window(); return 0; }
        if (self && message == WM_NCDESTROY)
        {
            self->shown = false;
            self->window = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void hide_window() noexcept
    {
        if (window && shown) ShowWindow(window, SW_HIDE);
        shown = false;
    }

    void failed(DWORD fallback = ERROR_GEN_FAILURE) noexcept
    {
        const DWORD detail = GetLastError();
        error = detail == ERROR_SUCCESS ? fallback : detail;
        retry_after = Clock::now() + std::chrono::seconds(1);
        hide_window();
    }

    bool ensure_window()
    {
        if (window) return true;
        if (!atom)
        {
            instance = GetModuleHandleW(nullptr);
            class_name = L"HmiCaptureOverlay_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(this));
            WNDCLASSEXW type{};
            type.cbSize = sizeof(type);
            type.hInstance = instance;
            type.lpfnWndProc = window_proc;
            type.lpszClassName = class_name.c_str();
            atom = RegisterClassExW(&type);
            if (!atom) { failed(); return false; }
        }
        constexpr DWORD extended = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED;
        if (!CreateWindowExW(extended, class_name.c_str(), L"语音采集", WS_POPUP, 0, 0, 1, 1,
                nullptr, nullptr, instance, this))
        { failed(); return false; }
        return true;
    }

    UINT monitor_dpi(HMONITOR monitor, HWND foreground)
    {
        if (!dpi_attempted)
        {
            dpi_attempted = true;
            dpi_module = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (dpi_module)
            {
                const FARPROC address = GetProcAddress(dpi_module, "GetDpiForMonitor");
                static_assert(sizeof(address) == sizeof(dpi_for_monitor));
                std::memcpy(&dpi_for_monitor, &address, sizeof(address));
            }
        }
        UINT x = 96, y = 96;
        if (dpi_for_monitor && SUCCEEDED(dpi_for_monitor(monitor, 0, &x, &y)) && x != 0)
            return std::clamp(x, 96U, 768U);
        const UINT fallback = foreground ? GetDpiForWindow(foreground) : 0;
        return std::clamp(fallback == 0 ? 96U : fallback, 96U, 768U);
    }

    bool ensure_fonts(UINT dpi)
    {
        if (font && clock_font && font_dpi == dpi) return true;
        const int height = -MulDiv(13, static_cast<int>(dpi), 96);
        const HFONT body = CreateFontW(height, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        const HFONT clock = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
        if (!body || !clock)
        {
            if (body) DeleteObject(body);
            if (clock) DeleteObject(clock);
            failed(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
        if (font) DeleteObject(font);
        if (clock_font) DeleteObject(clock_font);
        font = body;
        clock_font = clock;
        font_dpi = dpi;
        return true;
    }

    void draw(const base::CapturePresentation &value, int width, int height, UINT dpi, double time)
    {
        buffer.clear(); // Pure black is the transparency key, including rounded corners.
        const HDC dc = buffer.dc();
        const SelectedObject brush(dc, GetStockObject(DC_BRUSH));
        const SelectedObject pen(dc, GetStockObject(DC_PEN));
        const SelectedObject body_font(dc, font);
        SetBkMode(dc, TRANSPARENT);
        const float scale = static_cast<float>(dpi) / 96.0f;
        const auto px = [scale](float logical) { return static_cast<int>(std::lround(logical * scale)); };
        constexpr COLORREF background = RGB(20, 29, 43);
        constexpr COLORREF foreground = RGB(232, 240, 249);
        constexpr COLORREF subdued = RGB(164, 181, 204);
        const int limit = std::clamp(value.limit_seconds, 1, 600);
        const double seconds = std::clamp(static_cast<double>(value.captured_frames) / 16000.0, 0.0,
            static_cast<double>(limit));
        const bool recording = value.phase == base::CapturePhase::Recording;
        const bool awaiting = value.phase == base::CapturePhase::AwaitingRelease;
        const bool ending = awaiting || (recording && static_cast<double>(limit) - seconds <= 5.0);
        const COLORREF accent = ending ? RGB(255, 206, 122) : RGB(122, 230, 188);
        rounded(dc, 1, 1, std::max(1, width - 2), std::max(1, height - 2), px(14), background, RGB(60, 80, 105));
        if (recording || awaiting)
        {
            const int center = px(25);
            microphone(dc, px(19), center, scale, accent);
            const wchar_t *label = awaiting ? L"请松开快捷键" : L"正在采集";
            const int label_width = text_width(dc, label);
            text(dc, label, px(35), px(10), label_width + px(2), px(30), foreground);
            const int elapsed = static_cast<int>(seconds);
            wchar_t clock[48]{};
            std::swprintf(clock, std::size(clock), L"%02d:%02d / %02d:%02d", elapsed / 60, elapsed % 60, limit / 60, limit % 60);
            int clock_width = 0;
            {
                const SelectedObject selected(dc, clock_font);
                clock_width = text_width(dc, clock);
                text(dc, clock, width - px(14) - clock_width, px(10), clock_width, px(30), subdued);
            }
            const int start = px(49) + label_width;
            const int end = width - clock_width - px(28);
            const int pitch = std::max(1, px(6));
            const int count = std::clamp((end - start) / pitch, 0, 64);
            for (int index = 0; index < count; ++index)
            {
                const auto sample = levels.size() - static_cast<std::size_t>(count) + static_cast<std::size_t>(index);
                const int bar_height = std::max(px(3), px(3.0f + 21.0f * levels[sample]));
                const int x = end - (count - index) * pitch;
                const COLORREF color = blend(background, accent, 0.25f + 0.65f *
                    (static_cast<float>(index) + 1.0f) / static_cast<float>(count));
                rounded(dc, x, center - bar_height / 2, std::max(1, px(3)), bar_height, std::max(1, px(1.5f)), color, color);
            }
        }
        else
        {
            const wchar_t *label = value.phase == base::CapturePhase::Preparing ? L"正在准备录音" : L"正在整理文字";
            text(dc, label, px(25), px(11), std::max(1, width - px(110)), px(34), foreground);
            for (int index = 0; index < 3; ++index)
            {
                const float pulse = .5f + .5f * static_cast<float>(std::sin(time * 4.0 - index * .9));
                circle(dc, width - px(62.0f - static_cast<float>(index) * 14.0f), px(28),
                    std::max(1, px(2.5f + pulse)), blend(background, RGB(165, 202, 255), .25f + .75f * pulse));
            }
        }
        const int track_width = std::max(0, width - px(28));
        const int track_y = height - px(40);
        const int track_height = std::max(1, px(28));
        rounded(dc, px(14), track_y, track_width, track_height, px(6), RGB(30, 43, 59), RGB(30, 43, 59));
        if (recording || awaiting)
        {
            const int filled = static_cast<int>(std::lround(static_cast<double>(track_width) * seconds / limit));
            const COLORREF progress_color = blend(RGB(30, 43, 59), accent, 0.12f);
            if (filled > 0)
                rounded(dc, px(14), track_y, filled, track_height, px(6), progress_color, progress_color);
        }
        transcript_text(dc, value, px(24), track_y, std::max(0, track_width - px(20)), track_height,
            value.transcript[0] ? foreground : subdued);
    }

    void update(const base::CapturePresentation &value)
    {
        if (value.phase == base::CapturePhase::Hidden)
        {
            hide_window();
            phase = value.phase;
            updated = {};
            retry_after = {};
            error = ERROR_SUCCESS;
            return;
        }
        const auto now = Clock::now();
        const bool changed = request_id != value.request_id || phase != value.phase;
        if (request_id != value.request_id) levels.fill(0.0f);
        request_id = value.request_id;
        phase = value.phase;
        const float delta = updated == Clock::time_point{} ? 1.0f / 60.0f :
            std::clamp(std::chrono::duration<float>(now - updated).count(), 0.0f, 0.1f);
        updated = now;
        if (now < retry_after) return;
        if (phase == base::CapturePhase::Recording || phase == base::CapturePhase::AwaitingRelease)
        {
            const bool live = value.captured_frames % 1440 != 0;
            for (std::size_t index = 0; index < levels.size(); ++index)
            {
                const float source = live ? (index + 1 < levels.size() ? value.levels[index + 1] : value.live_level) : value.levels[index];
                const float target = level_amount(source);
                const float rate = target > levels[index] ? 28.0f : 9.0f;
                if (phase == base::CapturePhase::Recording) levels[index] += (target - levels[index]) * (1.0f - std::exp(-rate * delta));
                else levels[index] = target;
            }
        }
        if (!ensure_window()) return;
        const HWND foreground = GetForegroundWindow();
        const HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!monitor || !GetMonitorInfoW(monitor, &info)) { failed(); return; }
        const UINT dpi = monitor_dpi(monitor, foreground);
        const int work_width = std::max(1, static_cast<int>(info.rcWork.right - info.rcWork.left));
        const int work_height = std::max(1, static_cast<int>(info.rcWork.bottom - info.rcWork.top));
        const int margin = std::min({MulDiv(24, static_cast<int>(dpi), 96), work_width / 4, work_height / 4});
        const int width = std::max(1, std::min(MulDiv(400, static_cast<int>(dpi), 96), work_width - margin * 2));
        const int height = std::max(1, std::min(MulDiv(88, static_cast<int>(dpi), 96), work_height - margin * 2));
        POINT destination{info.rcWork.left + (work_width - width) / 2, info.rcWork.bottom - height - margin};
        const bool geometry = dimensions.cx != width || dimensions.cy != height || location.x != destination.x ||
            location.y != destination.y || previous_dpi != dpi;
        const double paint_interval = value.phase == base::CapturePhase::Recording ? 1.0 / 60.0 : 1.0 / 30.0;
        if (shown && !changed && !geometry && painted != Clock::time_point{} &&
            std::chrono::duration<double>(now - painted).count() < paint_interval) return;
        if (!ensure_fonts(dpi)) return;
        ScreenDc screen;
        if (!screen.value || !buffer.resize(screen.value, width, height)) { failed(ERROR_NOT_ENOUGH_MEMORY); return; }
        draw(value, width, height, dpi, std::chrono::duration<double>(now.time_since_epoch()).count());
        GdiFlush();
        SIZE size{width, height};
        POINT source{};
        if (!UpdateLayeredWindow(window, screen.value, &destination, &size, buffer.dc(), &source,
                RGB(0, 0, 0), nullptr, ULW_COLORKEY))
        { failed(); return; }
        if (!SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER))
        { failed(); return; }
        if (!shown) ShowWindow(window, SW_SHOWNOACTIVATE);
        shown = true;
        location = destination;
        dimensions = size;
        previous_dpi = dpi;
        painted = now;
        error = ERROR_SUCCESS;
    }
};

CaptureOverlay::CaptureOverlay() : impl_(std::make_unique<Impl>()) {}
CaptureOverlay::~CaptureOverlay() = default;
void CaptureOverlay::update(const base::CapturePresentation &presentation) { impl_->update(presentation); }
void CaptureOverlay::hide() noexcept { impl_->hide_window(); }
bool CaptureOverlay::visible() const noexcept { return impl_->shown; }
std::uint32_t CaptureOverlay::last_error() const noexcept { return impl_->error; }
}
