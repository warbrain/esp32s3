#include "action/ui_input.hpp"
#include "action/input_observation.hpp"
#include "action/clipboard_snapshot.hpp"

#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <oleauto.h>
#include <oleacc.h>
#include <UIAutomationClient.h>
#include <UIAutomationCoreApi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace hmi::action
{
namespace
{
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point started)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

InsertResult finish_input(InsertResult result, Clock::time_point started)
{
    result.total_ms = elapsed_ms(started);
    if (result.delivery == InputDelivery::NotSent)
    {
        if (result.ok) result.delivery = InputDelivery::DeliveredUnverified;
        else if (result.send_input_attempted) result.delivery = InputDelivery::Unknown;
    }
    return result;
}

struct SendInputObservation
{
    double elapsed_ms = -1.0;
    std::uint32_t calls = 0;
};

UINT measured_send_input(UINT count, INPUT *events, SendInputObservation &observation)
{
    const DWORD incoming_error = GetLastError();
    const auto started = Clock::now();
    SetLastError(incoming_error);
    const UINT sent = SendInput(count, events, sizeof(INPUT));
    const DWORD error = GetLastError();
    const double duration = elapsed_ms(started);
    observation.elapsed_ms = observation.calls == 0 ? duration : observation.elapsed_ms + duration;
    ++observation.calls;
    SetLastError(error);
    return sent;
}

void copy_send_input_observation(InsertResult &result, const SendInputObservation &observation)
{
    result.send_input_ms = observation.elapsed_ms;
    result.send_input_calls = observation.calls;
    result.send_input_attempted = observation.calls != 0;
}

bool valid_routing(std::string_view routing)
{
    return routing == "auto" || routing == "uia" || routing == "sendinput";
}

std::string code_detail(const char *operation, unsigned long code)
{
    char text[160]{};
    std::snprintf(text, sizeof(text), "%s failed (0x%08lx)", operation, code);
    return text;
}

std::wstring wide(std::string_view value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

struct WindowIdentity
{
    HWND window = nullptr;
    DWORD pid = 0;
    HWND focus = nullptr;
};

bool foreground_matches(WindowIdentity target)
{
    if (!target.window || target.pid == 0 || GetForegroundWindow() != target.window) return false;
    DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(target.window, &pid);
    if (pid != target.pid) return false;
    if (target.focus)
    {
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (!GetGUIThreadInfo(thread, &info) || info.hwndFocus != target.focus) return false;
    }
    return true;
}

HWND native_focus(WindowIdentity target)
{
    DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(target.window, &pid);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!thread || pid != target.pid || !GetGUIThreadInfo(thread, &info)) return nullptr;
    return info.hwndFocus;
}

std::uint32_t pressed_modifiers()
{
    std::uint32_t result = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) result |= 1;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) result |= 2;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) result |= 4;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) result |= 8;
    return result;
}

void describe_control(InsertResult &result, WindowIdentity target)
{
    result.modifiers = pressed_modifiers();
    result.modifiers_observed = true;
    const HWND focus = target.focus ? target.focus : native_focus(target);
    if (!focus || !IsWindow(focus)) return;
    wchar_t name[256]{};
    const int length = GetClassNameW(focus, name, 256);
    if (length > 0) result.control_class = utf8(std::wstring_view(name, static_cast<std::size_t>(length)));
    result.control_unicode = IsWindowUnicode(focus) ? 1 : 0;
}

bool legacy_editable(DWORD role, DWORD state)
{
    return role == ROLE_SYSTEM_TEXT && (state & (STATE_SYSTEM_READONLY | STATE_SYSTEM_UNAVAILABLE)) == 0;
}

bool definitely_not_written(HRESULT status)
{
    // These provider responses explicitly reject the operation. Other errors
    // (including transport failures/timeouts) cannot prove that no write ran.
    return status == E_NOTIMPL || status == UIA_E_NOTSUPPORTED;
}

bool clipboard_input_process(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768]{};
    DWORD length = 32768;
    const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!queried) return false;
    const std::wstring_view full(path, length);
    const auto slash = full.find_last_of(L"\\/");
    const std::wstring name(full.substr(slash == std::wstring_view::npos ? 0 : slash + 1));
    return _wcsicmp(name.c_str(), L"Weixin.exe") == 0 || _wcsicmp(name.c_str(), L"WeChat.exe") == 0 ||
        _wcsicmp(name.c_str(), L"Notepad.exe") == 0 || _wcsicmp(name.c_str(), L"wordpad.exe") == 0 ||
        _wcsicmp(name.c_str(), L"WINWORD.EXE") == 0 || _wcsicmp(name.c_str(), L"EXCEL.EXE") == 0;
}

bool paste_keys_released()
{
    for (const int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN, static_cast<int>('V')})
        if (GetAsyncKeyState(key) & 0x8000) return false;
    return true;
}

class ClipboardAccess
{
public:
    explicit ClipboardAccess(HWND window) : opened_(OpenClipboard(window) != FALSE) {}
    ClipboardAccess(const ClipboardAccess &) = delete;
    ClipboardAccess &operator=(const ClipboardAccess &) = delete;
    ~ClipboardAccess() { close(); }
    explicit operator bool() const noexcept { return opened_; }
    bool open(HWND window)
    {
        if (!opened_) opened_ = OpenClipboard(window) != FALSE;
        return opened_;
    }
    void close() noexcept
    {
        if (opened_) { CloseClipboard(); opened_ = false; }
    }

private:
    bool opened_ = false;
};

struct ClipboardItem
{
    UINT format = 0;
    HANDLE data = nullptr;

    ClipboardItem(UINT value, HANDLE copy) : format(value), data(copy) {}
    ClipboardItem(const ClipboardItem &) = delete;
    ClipboardItem &operator=(const ClipboardItem &) = delete;
    ClipboardItem(ClipboardItem &&other) noexcept : format(other.format), data(other.data) { other.data = nullptr; }
    ~ClipboardItem()
    {
        if (!data) return;
        if (format == CF_BITMAP || format == CF_DSPBITMAP || format == CF_PALETTE) DeleteObject(data);
        else if (format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE) DeleteEnhMetaFile(static_cast<HENHMETAFILE>(data));
        else
        {
            if (format == CF_METAFILEPICT || format == CF_DSPMETAFILEPICT)
            {
                const auto picture = static_cast<METAFILEPICT *>(GlobalLock(data));
                if (picture) { DeleteMetaFile(picture->hMF); GlobalUnlock(data); }
            }
            GlobalFree(data);
        }
    }
};

bool independent_clipboard_format(UINT format)
{
    if (format == CF_OWNERDISPLAY || (format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST) ||
        (format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST)) return false;
    if (format < 0xc000) return true;
    // Registered formats use HGLOBAL payloads. Keep known OLE transport
    // records with owner references out of this byte-only snapshot.
    wchar_t name[128]{};
    if (!GetClipboardFormatNameW(format, name, 128)) return false;
    return _wcsicmp(name, L"DataObject") != 0 && _wcsicmp(name, L"Ole Private Data") != 0;
}

class ClipboardPaste
{
public:
    explicit ClipboardPaste(WindowIdentity target, detail::ClipboardSnapshotWorker *snapshots = nullptr) :
        target_(target), snapshots_(snapshots) {}
    ClipboardPaste(const ClipboardPaste &) = delete;
    ClipboardPaste &operator=(const ClipboardPaste &) = delete;
    ~ClipboardPaste()
    {
        if (published_)
        {
            if (!submitted_) (void)restore();
            else (void)materialize();
        }
        if (window_) DestroyWindow(window_);
        if (atom_) UnregisterClassW(class_name_.c_str(), instance_);
        if (text_) GlobalFree(text_);
    }

    void copy_observation(InsertResult &result) const
    {
        copy_send_input_observation(result, send_input_);
    }

    InsertResult insert(const std::wstring &text)
    {
        const auto fail = [this](std::string detail) {
            if (!snapshot_detail_.empty()) detail += "; " + snapshot_detail_;
            if (!restore_detail_.empty()) detail += "; " + restore_detail_;
            return InsertResult{false, InputPath::SendInput, std::move(detail)};
        };
        if (!foreground_matches(target_) || !paste_keys_released())
            return fail("Unicode paste cancelled: foreground changed or paste keys held");
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        text_ = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!text_) return fail("Unicode paste: cannot allocate Unicode clipboard text");
        void *destination = GlobalLock(text_);
        if (!destination) return fail("Unicode paste: cannot lock Unicode clipboard text");
        std::memcpy(destination, text.c_str(), bytes);
        GlobalUnlock(text_);
        instance_ = GetModuleHandleW(nullptr);
        class_name_ = L"HmiClipboardPaste_" + std::to_wstring(reinterpret_cast<std::uintptr_t>(this));
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance_;
        window_class.lpszClassName = class_name_.c_str();
        atom_ = RegisterClassW(&window_class);
        if (!atom_) return fail("Unicode paste: cannot register clipboard owner");
        window_ = CreateWindowExW(0, class_name_.c_str(), L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, instance_, this);
        if (!window_) return fail("Unicode paste: cannot create clipboard owner");
        ClipboardAccess clipboard(window_);
        if (!clipboard) return fail("Unicode paste: clipboard is busy; no input sent");
        const auto snapshot_started = Clock::now();
        const DWORD original_sequence = GetClipboardSequenceNumber();
        bool saved = snapshot();
        if (saved && requires_ole_)
        {
            clipboard.close();
            if (!snapshots_) { saved = false; snapshot_error_ = "ole_snapshot requires a persistent input owner"; }
            else saved = snapshots_->capture(native_formats_, ole_snapshot_, snapshot_error_);
            if (saved && !clipboard.open(window_))
            { saved = false; snapshot_error_ = "ole_snapshot clipboard became busy before publish"; }
            if (saved && GetClipboardSequenceNumber() != original_sequence)
            { saved = false; snapshot_error_ = "ole_snapshot clipboard changed during capture; no input sent"; }
        }
        snapshot_detail_ = "snapshot_ms=" + std::to_string(elapsed_ms(snapshot_started)) +
            " snapshot=" + (requires_ole_ ? "ole" : "native");
        if (!saved || !foreground_matches(target_) || !paste_keys_released())
        {
            clipboard.close();
            return fail(saved ? "Unicode paste cancelled before clipboard change" :
                "Unicode paste: clipboard cannot be fully preserved; no input sent; " + snapshot_error_);
        }
        if (!EmptyClipboard()) { clipboard.close(); return fail("Unicode paste: cannot prepare clipboard"); }
        published_ = true;
        SetLastError(ERROR_SUCCESS);
        SetClipboardData(CF_UNICODETEXT, nullptr);
        const bool advertised = GetLastError() == ERROR_SUCCESS && IsClipboardFormatAvailable(CF_UNICODETEXT);
        sequence_ = GetClipboardSequenceNumber();
        if (!advertised)
        {
            bool restored = false;
            if (ole_snapshot_) { clipboard.close(); restored = restore(); }
            else { restored = restore_saved_locked(); clipboard.close(); }
            return fail(restored ? "Unicode paste: cannot publish Unicode text; clipboard restored" :
                "Unicode paste: cannot publish Unicode text; clipboard restoration incomplete");
        }
        clipboard.close();
        if (!foreground_matches(target_) || !paste_keys_released())
        {
            (void)restore();
            return fail("Unicode paste cancelled before key delivery");
        }
        INPUT keys[4]{};
        for (auto &key : keys) key.type = INPUT_KEYBOARD;
        keys[0].ki.wVk = VK_CONTROL;
        keys[1].ki.wVk = 'V';
        keys[2].ki.wVk = 'V'; keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
        keys[3].ki.wVk = VK_CONTROL; keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
        submitted_ = true;
        const UINT sent = measured_send_input(4, keys, send_input_);
        if (sent < 2) submitted_ = false;
        if (sent != 4)
        {
            // Release only; a possibly delivered paste must never be retried.
            measured_send_input(2, keys + 2, send_input_);
            if (!submitted_) { (void)restore(); return fail("Unicode paste: key delivery failed; not retried"); }
        }
        const ULONGLONG deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline)
        {
            MSG message{};
            for (unsigned index = 0; index < 32 && PeekMessageW(&message, window_, 0, 0, PM_REMOVE); ++index)
                DispatchMessageW(&message);
            if (consumed_ && restore())
                return {sent == 4, InputPath::SendInput, sent == 4 ?
                    "Unicode paste consumed; clipboard restored; text not visually verified; " + snapshot_detail_ :
                    "Unicode paste may have completed after partial key delivery; not retried"};
            if (!published_)
                return fail(restore_failed_ ? "Unicode paste consumed; clipboard restoration incomplete; not retried" :
                    "Unicode paste outcome unknown: clipboard changed; no input retried");
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        const bool available = materialize();
        return fail(available ?
            "Unicode paste outcome unknown: target read not confirmed; Unicode text kept on clipboard; not retried" :
            "Unicode paste outcome unknown: clipboard could not be completed; not retried");
    }

private:
    bool snapshot_failure(UINT format, const char *stage, DWORD error = ERROR_SUCCESS, SIZE_T bytes = 0)
    {
        wchar_t name[256]{};
        const int length = format >= 0xc000 ? GetClipboardFormatNameW(format, name, 256) : 0;
        char details[160]{};
        std::snprintf(details, sizeof(details), "format=%u stage=%s error=0x%08lx bytes=%zu",
            format, stage, error, static_cast<std::size_t>(bytes));
        snapshot_error_ = std::string(details) + " format_name=" +
            (length > 0 ? utf8(std::wstring_view(name, static_cast<std::size_t>(length))) : "standard_or_private");
        return false;
    }

    bool snapshot()
    {
        snapshot_error_.clear();
        native_formats_.clear();
        requires_ole_ = false;
        UINT format = 0;
        for (;;)
        {
            SetLastError(ERROR_SUCCESS);
            format = EnumClipboardFormats(format);
            if (!format)
            {
                const DWORD error = GetLastError();
                if (error != ERROR_SUCCESS) return snapshot_failure(0, "enumerate", error);
                break;
            }
            if (native_formats_.size() >= 64) return snapshot_failure(format, "format_count_limit");
            native_formats_.push_back(format);
            requires_ole_ = requires_ole_ || detail::ole_transport_format(format);
        }
        if (requires_ole_) return true;
        constexpr std::uint64_t maximum_bytes = 32ULL * 1024 * 1024;
        std::uint64_t total = 0;
        for (const UINT saved_format : native_formats_)
        {
            format = saved_format;
            if (!independent_clipboard_format(format))
                return snapshot_failure(format, "unsupported_format");
            SetLastError(ERROR_SUCCESS);
            HANDLE source = GetClipboardData(format);
            if (!source) return snapshot_failure(format, "read", GetLastError());
            SetLastError(ERROR_SUCCESS);
            const std::uint64_t budget = maximum_bytes - total;
            std::uint64_t bytes = 0;
            if (format == CF_BITMAP || format == CF_DSPBITMAP)
            {
                BITMAP bitmap{};
                if (GetObjectW(source, static_cast<int>(sizeof(bitmap)), &bitmap) != static_cast<int>(sizeof(bitmap)) ||
                    bitmap.bmHeight <= 0 || bitmap.bmWidth <= 0 || bitmap.bmWidthBytes <= 0 ||
                    bitmap.bmPlanes == 0 || bitmap.bmBitsPixel == 0)
                    return snapshot_failure(format, "bitmap_size", GetLastError());
                bytes = static_cast<std::uint64_t>(bitmap.bmHeight);
                const auto stride = static_cast<std::uint64_t>(bitmap.bmWidthBytes);
                if (bytes > budget / stride) return snapshot_failure(format, "size_limit");
                bytes *= stride;
                if (bytes > budget / bitmap.bmPlanes) return snapshot_failure(format, "size_limit");
                bytes *= bitmap.bmPlanes;
            }
            else if (format == CF_PALETTE)
            {
                const UINT entries = GetPaletteEntries(static_cast<HPALETTE>(source), 0, 0, nullptr);
                if (entries == 0) return snapshot_failure(format, "palette_size", GetLastError());
                constexpr std::uint64_t header_bytes = sizeof(LOGPALETTE) - sizeof(PALETTEENTRY);
                if (header_bytes > budget || entries > (budget - header_bytes) / sizeof(PALETTEENTRY))
                    return snapshot_failure(format, "size_limit");
                bytes = header_bytes + static_cast<std::uint64_t>(entries) * sizeof(PALETTEENTRY);
            }
            else if (format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE)
                bytes = GetEnhMetaFileBits(static_cast<HENHMETAFILE>(source), 0, nullptr);
            else if (format == CF_METAFILEPICT || format == CF_DSPMETAFILEPICT)
            {
                const SIZE_T header_bytes = GlobalSize(source);
                if (header_bytes < sizeof(METAFILEPICT))
                    return snapshot_failure(format, "metafile_size", GetLastError());
                if (header_bytes > budget) return snapshot_failure(format, "size_limit", ERROR_SUCCESS, header_bytes);
                const auto *picture = static_cast<METAFILEPICT *>(GlobalLock(source));
                if (!picture) return snapshot_failure(format, "lock", GetLastError());
                const UINT payload_bytes = GetMetaFileBitsEx(picture->hMF, 0, nullptr);
                GlobalUnlock(source);
                if (payload_bytes == 0) return snapshot_failure(format, "metafile_size", GetLastError());
                if (payload_bytes > budget - header_bytes) return snapshot_failure(format, "size_limit");
                bytes = static_cast<std::uint64_t>(header_bytes) + payload_bytes;
            }
            else bytes = GlobalSize(source);
            if (bytes == 0) return snapshot_failure(format, "memory_size", GetLastError());
            if (bytes > budget)
                return snapshot_failure(format, "size_limit", ERROR_SUCCESS, static_cast<SIZE_T>(bytes));
            total += bytes;
            HANDLE copy = nullptr;
            SetLastError(ERROR_SUCCESS);
            if (format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE)
                copy = CopyEnhMetaFileW(static_cast<HENHMETAFILE>(source), nullptr);
            else
            {
                const UINT base_format = format == CF_DSPBITMAP ? CF_BITMAP :
                    format == CF_DSPMETAFILEPICT ? CF_METAFILEPICT : format;
                copy = OleDuplicateData(source, static_cast<CLIPFORMAT>(base_format), 0);
            }
            if (!copy) return snapshot_failure(format, "duplicate", GetLastError());
            ClipboardItem owned(format, copy);
            saved_.push_back(std::move(owned));
        }
        return true;
    }

    bool restore()
    {
        if (!published_) return false;
        ClipboardAccess clipboard(window_);
        if (!clipboard) return false;
        if (GetClipboardOwner() != window_ || GetClipboardSequenceNumber() != sequence_)
        {
            published_ = false;
            return false;
        }
        if (ole_snapshot_)
        {
            if (ole_restore_attempted_) return false;
            ole_restore_attempted_ = true;
            clipboard.close();
            const auto result = snapshots_ ? snapshots_->restore(ole_snapshot_, window_, sequence_, restore_detail_) :
                detail::ClipboardRestoreResult::Failed;
            const bool restored = result == detail::ClipboardRestoreResult::Restored;
            if (result == detail::ClipboardRestoreResult::Failed) (void)materialize();
            published_ = false;
            restore_failed_ = !restored;
            return restored;
        }
        const bool restored = restore_saved_locked();
        return restored;
    }

    bool restore_saved_locked()
    {
        if (ole_snapshot_) return false; // OLE restoration must run after closing the Win32 clipboard lock.
        bool restored = EmptyClipboard() != FALSE;
        if (restored)
        {
            published_ = false;
            for (auto &item : saved_)
            {
                if (SetClipboardData(item.format, item.data)) item.data = nullptr;
                else restored = false;
            }
        }
        restore_failed_ = !restored;
        return restored;
    }

    bool materialize()
    {
        if (!published_) return false;
        ClipboardAccess clipboard(window_);
        if (!clipboard) return false;
        if (GetClipboardOwner() != window_ || GetClipboardSequenceNumber() != sequence_)
        {
            published_ = false;
            return false;
        }
        render(false);
        return published_ && !text_;
    }

    void render(bool target_request)
    {
        if (!text_ || !published_ || GetClipboardOwner() != window_) return;
        if (SetClipboardData(CF_UNICODETEXT, text_))
        {
            text_ = nullptr;
            sequence_ = GetClipboardSequenceNumber();
            consumed_ = submitted_ && target_request;
        }
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto *self = reinterpret_cast<ClipboardPaste *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<ClipboardPaste *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = window;
        }
        if (self)
        {
            if (message == WM_RENDERFORMAT && wparam == CF_UNICODETEXT)
            {
                DWORD reader = 0;
                if (const HWND opener = GetOpenClipboardWindow()) GetWindowThreadProcessId(opener, &reader);
                self->render(reader == self->target_.pid && foreground_matches(self->target_));
                return 0;
            }
            if (message == WM_RENDERALLFORMATS) { (void)self->materialize(); return 0; }
            if (message == WM_DESTROYCLIPBOARD) { self->published_ = false; return 0; }
            if (message == WM_NCDESTROY) SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    WindowIdentity target_;
    detail::ClipboardSnapshotWorker *snapshots_ = nullptr;
    HINSTANCE instance_ = nullptr;
    ATOM atom_ = 0;
    HWND window_ = nullptr;
    std::wstring class_name_;
    HGLOBAL text_ = nullptr;
    std::vector<ClipboardItem> saved_;
    std::vector<UINT> native_formats_;
    std::shared_ptr<const detail::ClipboardSnapshot> ole_snapshot_;
    std::string snapshot_error_;
    std::string snapshot_detail_;
    std::string restore_detail_;
    DWORD sequence_ = 0;
    bool published_ = false;
    bool submitted_ = false;
    bool consumed_ = false;
    bool restore_failed_ = false;
    bool requires_ole_ = false;
    bool ole_restore_attempted_ = false;
    SendInputObservation send_input_;
};

InsertResult send_unicode(const std::wstring &text, WindowIdentity target)
{
    if (!foreground_matches(target))
        return {false, InputPath::None, "foreground window changed; input cancelled"};
    InsertResult result;
    describe_control(result, target);
    if (result.modifiers != 0)
    {
        result.detail = "modifier keys are still held; no input sent";
        return result;
    }
    std::vector<INPUT> events;
    events.reserve(text.size() * 2);
    for (const wchar_t ch : text)
    {
        INPUT event{};
        event.type = INPUT_KEYBOARD;
        event.ki.wScan = ch;
        event.ki.dwFlags = KEYEVENTF_UNICODE;
        events.push_back(event);
        event.ki.dwFlags |= KEYEVENTF_KEYUP;
        events.push_back(event);
    }
    if (!foreground_matches(target))
        return {false, InputPath::None, "foreground window changed; input cancelled"};
    result.modifiers = pressed_modifiers();
    if (result.modifiers != 0)
    {
        result.detail = "modifier keys changed before delivery; no input sent";
        return result;
    }
    SendInputObservation observation;
    const UINT sent = measured_send_input(static_cast<UINT>(events.size()), events.data(), observation);
    const DWORD error = sent == events.size() ? ERROR_SUCCESS : GetLastError();
    if (sent != events.size())
    {
        // Do not leave a Unicode key down after a partial insertion.
        std::vector<INPUT> release;
        release.reserve(text.size());
        for (std::size_t index = 1; index < events.size(); index += 2) release.push_back(events[index]);
        measured_send_input(static_cast<UINT>(release.size()), release.data(), observation);
    }
    result.ok = sent == events.size();
    result.path = InputPath::SendInput;
    result.delivery = result.ok ? InputDelivery::DeliveredUnverified : InputDelivery::Unknown;
    result.detail = result.ok ? "Unicode events delivered; target text not verified" :
        code_detail("SendInput (possibly blocked by UIPI)", error);
    copy_send_input_observation(result, observation);
    return result;
}

InsertResult send_text_fallback(const std::wstring &text, WindowIdentity target,
                                detail::ClipboardSnapshotWorker *snapshots = nullptr)
{
    if (clipboard_input_process(target.pid))
    {
        const auto started = Clock::now();
        InsertResult result;
        {
            ClipboardPaste paste(target, snapshots);
            result = paste.insert(text);
            paste.copy_observation(result);
        }
        result.clipboard_attempted = true;
        result.clipboard_ms = elapsed_ms(started);
        return result;
    }
    return send_unicode(text, target);
}

bool validate_text(std::string_view text, std::wstring &converted, InsertResult &error)
{
    if (text.empty() || text.size() > 16384 || text.find('\0') != std::string_view::npos)
    {
        error = {false, InputPath::None, "text must contain 1..16384 UTF-8 bytes without NUL"};
        return false;
    }
    converted = wide(text);
    if (!converted.empty()) return true;
    error = {false, InputPath::None, "invalid UTF-8"};
    return false;
}

struct Job
{
    std::promise<InsertResult> promise;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> writing{false};
    std::atomic<DWORD> thread_id{0};
};

struct AutomationString
{
    BSTR value = nullptr;
    ~AutomationString() { SysFreeString(value); }
};

InsertResult attempt_uia(const std::wstring &text, WindowIdentity target, const std::shared_ptr<Job> &job)
{
    ComPtr<IUIAutomation> automation;
    HRESULT status = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&automation));
    if (FAILED(status))
        status = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&automation));
    if (FAILED(status))
        return {false, InputPath::None, code_detail("UI Automation creation", static_cast<unsigned long>(status))};
    ComPtr<IUIAutomation2> timed;
    if (SUCCEEDED(automation.As(&timed)))
    {
        timed->put_ConnectionTimeout(250);
        timed->put_TransactionTimeout(250);
    }
    ComPtr<IUIAutomationElement> window;
    ComPtr<IUIAutomationElement> focus;
    if (FAILED(automation->ElementFromHandle(target.window, &window)) ||
        FAILED(automation->GetFocusedElement(&focus)) || !focus)
        return {false, InputPath::None, "focused UIA element unavailable"};
    int focused_pid = 0;
    if (FAILED(focus->get_CurrentProcessId(&focused_pid)) ||
        static_cast<DWORD>(focused_pid) != target.pid || !foreground_matches(target))
        return {false, InputPath::None, "foreground window changed"};
    ComPtr<IUIAutomationValuePattern> value;
    ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
    BOOL readonly = TRUE;
    focus->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value));
    const bool writable = value && SUCCEEDED(value->get_CurrentIsReadOnly(&readonly)) && !readonly;
    focus->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&legacy));
    DWORD role = 0;
    DWORD state = 0;
    const bool legacy_writable = legacy && SUCCEEDED(legacy->get_CurrentRole(&role)) &&
        SUCCEEDED(legacy->get_CurrentState(&state)) && legacy_editable(role, state);
    const InputPath path = choose_input_path(writable, legacy_writable);
    if (job->cancelled.load() || !foreground_matches(target))
        return {false, InputPath::None, "UIA cancelled before write"};
    if (path == InputPath::SendInput)
        return {false, InputPath::None, "no writable UIA pattern"};
    AutomationString input{SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))};
    if (!input.value) return {false, InputPath::None, "cannot allocate UIA input string"};
    job->writing.store(true);
    if (job->cancelled.load() || !foreground_matches(target))
    {
        job->writing.store(false);
        return {false, InputPath::None, "UIA cancelled before write"};
    }
    status = path == InputPath::UiaValue ? value->SetValue(input.value)
                                        : legacy->SetValue(input.value);
    if (definitely_not_written(status)) job->writing.store(false);
    return {SUCCEEDED(status), path, SUCCEEDED(status) ? "focused control value set" :
        code_detail("UIA SetValue", static_cast<unsigned long>(status))};
}
}

InputPath choose_input_path(bool value_writable, bool legacy_writable) noexcept
{
    return choose_input_path(value_writable, legacy_writable, "auto");
}

InputPath choose_input_path(bool value_writable, bool legacy_writable, std::string_view routing) noexcept
{
    if (!valid_routing(routing)) return InputPath::None;
    if (routing == "sendinput") return InputPath::SendInput;
    if (value_writable) return InputPath::UiaValue;
    if (legacy_writable) return InputPath::UiaLegacy;
    // TextPattern2 has no insertion API. Text-only controls use Unicode input.
    return routing == "uia" ? InputPath::None : InputPath::SendInput;
}

struct TextInput::Impl
{
    std::mutex operation;
    detail::ClipboardSnapshotWorker clipboard_snapshots;
    detail::InputObservationWorker observations;
    std::shared_ptr<Job> job;
    std::future<InsertResult> result;
    std::thread worker;

    InsertResult deliver(const std::wstring &text, WindowIdentity target)
    {
        if (!target.focus) target.focus = native_focus(target);
        if (!foreground_matches(target))
            return {false, InputPath::None, "focused editor changed; no input sent"};
        InsertResult control;
        describe_control(control, target);
        detail::InputObservationWorker::Ticket ticket;
        if (!job) ticket = observations.begin(text, target.window, target.pid, target.focus);
        InsertResult outcome;
        try { outcome = send_text_fallback(text, target, &clipboard_snapshots); }
        catch (...)
        {
            (void)observations.finish(ticket, false);
            throw;
        }
        const auto evidence = observations.finish(ticket, outcome.ok);
        outcome.control_class = std::move(control.control_class);
        outcome.control_unicode = control.control_unicode;
        if (outcome.modifiers == 0) outcome.modifiers = control.modifiers;
        outcome.modifiers_observed = control.modifiers_observed;
        outcome.verification_attempted = static_cast<bool>(ticket);
        outcome.verification_before_ms = evidence.before_ms;
        outcome.verification_after_ms = evidence.after_ms;
        outcome.verification_reason = evidence.reason;
        outcome.font_name = utf8(evidence.font_name);
        if (outcome.ok)
        {
            outcome.delivery = InputDelivery::DeliveredUnverified;
            if (evidence.outcome == detail::VerificationOutcome::Verified)
            {
                outcome.delivery = InputDelivery::Verified;
                outcome.detail += "; exact target text insertion verified";
            }
            else if (evidence.outcome == detail::VerificationOutcome::Mismatch)
            {
                outcome.ok = false;
                outcome.delivery = InputDelivery::Mismatch;
                outcome.detail += "; target text differs from the requested insertion; not retried";
            }
        }
        return outcome;
    }

    void cancel() const noexcept
    {
        if (!job) return;
        job->cancelled.store(true);
        const DWORD thread = job->thread_id.load();
        if (thread != 0) CoCancelCall(thread, 0);
    }

    bool complete() const
    {
        return result.valid() && result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    }

    InsertResult collect()
    {
        if (worker.joinable()) worker.join();
        InsertResult value = result.get();
        job.reset();
        return value;
    }

    ~Impl()
    {
        cancel();
        if (!worker.joinable()) return;
        if (complete()) worker.join();
        else
        {
            // A provider can ignore COM cancellation. Only this one operation
            // survives; its thread captures the job and strings, never Impl.
            worker.detach();
        }
    }
};

TextInput::TextInput() : impl_(std::make_unique<Impl>()) {}
TextInput::~TextInput() = default;

Target foreground_target()
{
    Target result;
    const HWND window = GetForegroundWindow();
    result.native_window = reinterpret_cast<std::uintptr_t>(window);
    GetWindowThreadProcessId(window, &result.pid);
    result.native_focus = reinterpret_cast<std::uintptr_t>(native_focus({window, result.pid}));
    wchar_t title[1024]{};
    GetWindowTextW(window, title, 1024);
    result.window = utf8(title);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, result.pid);
    if (process)
    {
        wchar_t path[32768]{};
        DWORD length = 32768;
        if (QueryFullProcessImageNameW(process, 0, path, &length))
        {
            const std::wstring full(path, length);
            result.process = utf8(full.substr(full.find_last_of(L"\\/") + 1));
        }
        CloseHandle(process);
    }
    return result;
}

InsertResult insert_text(std::string_view text)
{
    const auto started = Clock::now();
    std::wstring converted;
    InsertResult invalid;
    if (!validate_text(text, converted, invalid)) return finish_input(std::move(invalid), started);
    const HWND target = GetForegroundWindow();
    if (!target) return finish_input({false, InputPath::None, "no foreground window"}, started);
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    return finish_input(send_text_fallback(converted, {target, pid}), started);
}

InsertResult TextInput::insert(std::string_view text, std::string_view routing)
{
    const auto started = Clock::now();
    if (!valid_routing(routing))
        return finish_input({false, InputPath::None, "target must be exactly auto, uia, or sendinput"}, started);
    std::wstring converted;
    InsertResult invalid;
    if (!validate_text(text, converted, invalid)) return finish_input(std::move(invalid), started);
    return finish_input(insert(text, foreground_target(), routing), started);
}

InsertResult TextInput::insert(std::string_view text, const Target &expected, std::string_view routing)
{
    const auto started = Clock::now();
    if (!valid_routing(routing))
        return finish_input({false, InputPath::None, "target must be exactly auto, uia, or sendinput"}, started);
    std::wstring converted;
    InsertResult invalid;
    if (!validate_text(text, converted, invalid)) return finish_input(std::move(invalid), started);
    std::lock_guard lock(impl_->operation);
    const WindowIdentity target{reinterpret_cast<HWND>(expected.native_window), expected.pid,
        reinterpret_cast<HWND>(expected.native_focus)};
    if (!foreground_matches(target))
        return finish_input({false, InputPath::None, "foreground window changed; input cancelled"}, started);
    if (impl_->observations.busy())
    {
        if (routing == "uia")
            return finish_input({false, InputPath::None, "previous readback still pending; forced UIA not started"}, started);
        auto delivered = impl_->deliver(converted, target);
        delivered.verification_reason = "previous_readback_pending";
        return finish_input(std::move(delivered), started);
    }
    if (impl_->job && impl_->complete()) (void)impl_->collect();
    if (impl_->job)
    {
        // A timed-out lookup is cancelled before it can write, so a later
        // request may use SendInput. Never overlap a potentially active write.
        if (impl_->job->writing.load())
            return finish_input({false, InputPath::None, "previous UIA write still pending; no new input sent"}, started);
        if (routing == "uia")
            return finish_input({false, InputPath::None, "previous UIA lookup still pending; forced UIA not retried"}, started);
        InsertResult fallback = impl_->deliver(converted, target);
        fallback.detail = "previous UIA lookup still pending; " + fallback.detail;
        return finish_input(std::move(fallback), started);
    }
    if (routing == "sendinput") return finish_input(impl_->deliver(converted, target), started);
    const auto uia_started = Clock::now();
    const auto finish_uia = [&](InsertResult result, double observed_ms, double worker_ms, bool timed_out = false) {
        result.uia_attempted = true;
        result.uia_ms = observed_ms;
        result.uia_worker_ms = worker_ms;
        result.uia_timed_out = timed_out;
        return finish_input(std::move(result), started);
    };
    const auto job = std::make_shared<Job>();
    impl_->job = job;
    impl_->result = job->promise.get_future();
    try
    {
        impl_->worker = std::thread([job, converted, target] {
        const auto worker_started = Clock::now();
        job->thread_id.store(GetCurrentThreadId());
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoEnableCallCancellation(nullptr);
        InsertResult attempt;
        try { attempt = job->cancelled.load() ? InsertResult{false, InputPath::None, "UIA cancelled"} :
                        SUCCEEDED(com) ? attempt_uia(converted, target, job)
                                       : InsertResult{false, InputPath::None, "COM initialization failed"}; }
        catch (...) { attempt = {false, InputPath::None, "UIA worker failed"}; }
        CoDisableCallCancellation(nullptr);
        if (SUCCEEDED(com)) CoUninitialize();
        attempt.uia_attempted = true;
        attempt.uia_worker_ms = elapsed_ms(worker_started);
        job->promise.set_value(std::move(attempt));
        });
    }
    catch (const std::exception &exception)
    {
        impl_->job.reset();
        return finish_uia({false, InputPath::None, std::string("cannot start UIA worker: ") + exception.what()},
                          elapsed_ms(uia_started), -1.0);
    }
    if (impl_->result.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready)
    {
        InsertResult attempt = impl_->collect();
        const double observed_ms = elapsed_ms(uia_started);
        const double worker_ms = attempt.uia_worker_ms;
        if (attempt.ok) return finish_uia(std::move(attempt), observed_ms, worker_ms);
        if (job->writing.load() || routing == "uia")
        {
            if (job->writing.load()) attempt.delivery = InputDelivery::Unknown;
            return finish_uia(std::move(attempt), observed_ms, worker_ms);
        }
        InsertResult fallback = impl_->deliver(converted, target);
        fallback.detail = attempt.detail + "; " + fallback.detail;
        return finish_uia(std::move(fallback), observed_ms, worker_ms);
    }
    impl_->cancel();
    const double observed_ms = elapsed_ms(uia_started);
    // Keep this one thread owned after timeout; subsequent calls cannot spawn
    // another. A late write must never race a fallback with the same text.
    if (job->writing.load())
    {
        InsertResult unknown{false, InputPath::None, "UIA write timed out; outcome unknown, not retried"};
        unknown.delivery = InputDelivery::Unknown;
        return finish_uia(std::move(unknown), observed_ms, -1.0, true);
    }
    if (routing == "uia")
        return finish_uia({false, InputPath::None, "UIA lookup timed out; forced UIA did not send input"},
                          observed_ms, -1.0, true);
    InsertResult fallback = impl_->deliver(converted, target);
    fallback.detail = "UIA lookup timed out; " + fallback.detail;
    return finish_uia(std::move(fallback), observed_ms, -1.0, true);
}
}
