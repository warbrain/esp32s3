#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <oleauto.h>
#include <oleacc.h>
#include <UIAutomationClient.h>
#include <UIAutomationCoreApi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace fake_input
{
std::atomic<std::uintptr_t> foreground{100};
std::atomic<DWORD> pid{42};
std::atomic<unsigned> foreground_calls{0};
std::atomic<unsigned> switch_at_call{0};
std::atomic<std::uintptr_t> focused_child{0};
std::atomic<unsigned> sends{0};
std::atomic<unsigned> event_count{0};
std::vector<INPUT> sent_events;
std::wstring process_image;
std::map<UINT, HANDLE> clipboard;
std::map<UINT, std::wstring> format_names;
WNDPROC window_proc = nullptr;
LONG_PTR window_data = 0;
HWND owner = reinterpret_cast<HWND>(800);
HWND opener = nullptr;
bool clipboard_open = false;
DWORD clipboard_sequence = 7;
unsigned empties = 0;
unsigned clipboard_opens = 0;
bool duplicate_failure = false;
bool clipboard_unavailable = false;
bool modifier_held = false;
bool paste_pending = false;
UINT accepted_events = 4;
std::atomic<ULONGLONG> tick{1000};
std::wstring consumed_text;
enum class PasteMode { Consume, Timeout, UserCopy, UnknownReader, ForeignReader, ReaderKeepsLock };
PasteMode paste_mode = PasteMode::Consume;
unsigned reader_waits = 0;
bool restored_before_reader_closed = false;
std::recursive_mutex clipboard_mutex;
Microsoft::WRL::ComPtr<IDataObject> ole_current;
std::atomic<unsigned> ole_gets{0}, ole_sets{0}, ole_flushes{0};
std::atomic<unsigned> ole_data_calls{0};
std::atomic<unsigned> ole_live_sources{0};
std::atomic<ULONGLONG> ole_source_stream_position{~0ULL};
std::atomic<bool> ole_get_blocked{false}, ole_get_release{false}, ole_get_waiting{false};
std::atomic<bool> ole_get_failure{false}, ole_unknown_medium{false}, ole_change_during_capture{false};
std::atomic<bool> ole_flush_blocked{false};
std::atomic<bool> oversized_medium{false};
BITMAP native_bitmap{};
UINT native_palette_entries = 0;
UINT native_enhanced_metafile_bytes = 0;
UINT native_metafile_bytes = 0;
unsigned native_media_copies = 0;
std::map<UINT, std::vector<unsigned char>> ole_streams;
constexpr UINT ole_marker = 0xc200;
HRESULT WINAPI ole_get_clipboard(IDataObject **out);
HRESULT WINAPI ole_set_clipboard(IDataObject *object);
HRESULT WINAPI ole_flush_clipboard();
HRESULT WINAPI ole_is_current(IDataObject *object);
HRESULT WINAPI ole_initialize(void *) { return S_OK; }
void WINAPI ole_uninitialize() {}
HRESULT WINAPI enable_cancellation(void *) { return S_OK; }
HRESULT WINAPI disable_cancellation(void *) { return S_OK; }
HRESULT WINAPI cancel_call(DWORD, ULONG) { return S_OK; }
SIZE_T WINAPI global_size(HGLOBAL memory)
{
    return oversized_medium.load() ? SIZE_T{32} * 1024 * 1024 + 1 : GlobalSize(memory);
}

HWND private_window() { return reinterpret_cast<HWND>(900); }

void free_clipboard()
{
    std::lock_guard lock(clipboard_mutex);
    for (const auto &[format, data] : clipboard)
    {
        (void)format;
        if (data) GlobalFree(data);
    }
    clipboard.clear();
}

HANDLE text_data(const std::wstring &text)
{
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return nullptr;
    void *value = GlobalLock(memory);
    if (!value) { GlobalFree(memory); return nullptr; }
    std::memcpy(value, text.c_str(), bytes);
    GlobalUnlock(memory);
    return memory;
}

HANDLE binary_data(const std::vector<unsigned char> &bytes)
{
    if (bytes.empty()) return nullptr;
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes.size());
    if (!memory) return nullptr;
    void *value = GlobalLock(memory);
    if (!value) { GlobalFree(memory); return nullptr; }
    std::memcpy(value, bytes.data(), bytes.size());
    GlobalUnlock(memory);
    return memory;
}

HANDLE native_media_data(UINT format)
{
    if (format != CF_METAFILEPICT && format != CF_DSPMETAFILEPICT) return binary_data({1, 2, 3, 4});
    METAFILEPICT picture{};
    picture.hMF = reinterpret_cast<HMETAFILE>(123);
    std::vector<unsigned char> bytes(sizeof(picture));
    std::memcpy(bytes.data(), &picture, sizeof(picture));
    return binary_data(bytes);
}

std::vector<unsigned char> clipboard_bytes(UINT format)
{
    const auto found = clipboard.find(format);
    if (found == clipboard.end() || !found->second) return {};
    const SIZE_T count = GlobalSize(found->second);
    const auto *value = static_cast<const unsigned char *>(GlobalLock(found->second));
    if (!value) return {};
    const std::vector<unsigned char> bytes(value, value + count);
    GlobalUnlock(found->second);
    return bytes;
}

std::wstring clipboard_text(UINT format = CF_UNICODETEXT)
{
    const auto found = clipboard.find(format);
    if (found == clipboard.end() || !found->second) return {};
    const wchar_t *value = static_cast<const wchar_t *>(GlobalLock(found->second));
    if (!value) return {};
    const std::wstring text(value);
    GlobalUnlock(found->second);
    return text;
}
HWND WINAPI foreground_window()
{
    const unsigned call = ++foreground_calls;
    if (switch_at_call.load() != 0 && call >= switch_at_call.load()) foreground = 200;
    return reinterpret_cast<HWND>(foreground.load());
}
DWORD WINAPI window_process(HWND window, LPDWORD value)
{
    *value = window == reinterpret_cast<HWND>(700) ? 99 : pid.load();
    return 1;
}
UINT WINAPI send(UINT count, LPINPUT events, int)
{
    ++sends;
    event_count += count;
    sent_events.insert(sent_events.end(), events, events + count);
    if (count == 4 && events[0].ki.wVk == VK_CONTROL && events[1].ki.wVk == 'V')
    {
        paste_pending = true;
        return accepted_events;
    }
    return count;
}
HRESULT WINAPI create_instance(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *) { return E_NOTIMPL; }
HANDLE WINAPI open_process(DWORD, BOOL, DWORD)
{
    return process_image.empty() ? nullptr : reinterpret_cast<HANDLE>(600);
}
BOOL WINAPI close_handle(HANDLE) { return TRUE; }
BOOL WINAPI process_image_name(HANDLE, DWORD, LPWSTR value, PDWORD size)
{
    if (process_image.empty() || *size <= process_image.size()) return FALSE;
    std::memcpy(value, process_image.c_str(), (process_image.size() + 1) * sizeof(wchar_t));
    *size = static_cast<DWORD>(process_image.size());
    return TRUE;
}
int WINAPI title(HWND, LPWSTR value, int) { *value = L'\0'; return 0; }
SHORT WINAPI key_state(int) { return modifier_held ? static_cast<SHORT>(0x8000) : 0; }
BOOL WINAPI gui_thread_info(DWORD, PGUITHREADINFO info)
{
    if (!info || info->cbSize != sizeof(*info)) return FALSE;
    info->hwndActive = reinterpret_cast<HWND>(foreground.load());
    info->hwndFocus = reinterpret_cast<HWND>(focused_child.load());
    return TRUE;
}
BOOL WINAPI is_window(HWND window) { return window != nullptr; }
BOOL WINAPI is_unicode_window(HWND) { return TRUE; }
int WINAPI class_name(HWND, LPWSTR name, int capacity)
{
    constexpr wchar_t value[] = L"FixtureEdit";
    if (capacity < static_cast<int>(std::size(value))) return 0;
    std::memcpy(name, value, sizeof(value));
    return static_cast<int>(std::size(value) - 1);
}

ATOM WINAPI register_class(const WNDCLASSW *value) { window_proc = value->lpfnWndProc; return 1; }
ATOM WINAPI register_class_ex(const WNDCLASSEXW *value) { window_proc = value->lpfnWndProc; return 1; }
BOOL WINAPI unregister_class(LPCWSTR, HINSTANCE) { window_proc = nullptr; return TRUE; }
LONG_PTR WINAPI set_window_data(HWND, int, LONG_PTR value)
{
    const LONG_PTR previous = window_data; window_data = value; return previous;
}
LONG_PTR WINAPI get_window_data(HWND, int) { return window_data; }
LRESULT WINAPI default_window_proc(HWND, UINT message, WPARAM, LPARAM)
{
    return message == WM_NCCREATE ? TRUE : 0;
}
HWND WINAPI create_window(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                          HWND, HMENU, HINSTANCE, LPVOID parameter)
{
    CREATESTRUCTW creation{};
    creation.lpCreateParams = parameter;
    if (!window_proc || !window_proc(private_window(), WM_NCCREATE, 0,
                                    reinterpret_cast<LPARAM>(&creation))) return nullptr;
    return private_window();
}
BOOL WINAPI destroy_window(HWND window)
{
    if (window_proc)
    {
        if (owner == window) window_proc(window, WM_RENDERALLFORMATS, 0, 0);
        window_proc(window, WM_DESTROY, 0, 0);
        window_proc(window, WM_NCDESTROY, 0, 0);
    }
    return TRUE;
}
BOOL WINAPI open_clipboard(HWND window)
{
    std::lock_guard lock(clipboard_mutex);
    ++clipboard_opens;
    if (clipboard_open || clipboard_unavailable) return FALSE;
    clipboard_open = true; opener = window; return TRUE;
}
BOOL WINAPI close_clipboard() { std::lock_guard lock(clipboard_mutex); clipboard_open = false; opener = nullptr; return TRUE; }
HWND WINAPI clipboard_owner() { std::lock_guard lock(clipboard_mutex); return owner; }
HWND WINAPI clipboard_opener() { std::lock_guard lock(clipboard_mutex); return opener; }
DWORD WINAPI clipboard_number() { std::lock_guard lock(clipboard_mutex); return clipboard_sequence; }
BOOL WINAPI empty_clipboard()
{
    std::lock_guard lock(clipboard_mutex);
    if (!clipboard_open) return FALSE;
    if (opener == reinterpret_cast<HWND>(100)) restored_before_reader_closed = true;
    ++empties;
    if (owner == private_window() && window_proc) window_proc(owner, WM_DESTROYCLIPBOARD, 0, 0);
    free_clipboard();
    ole_current.Reset();
    owner = opener;
    ++clipboard_sequence;
    return TRUE;
}
UINT WINAPI clipboard_formats(UINT previous)
{
    std::lock_guard lock(clipboard_mutex);
    const auto next = clipboard.upper_bound(previous);
    if (next == clipboard.end()) { SetLastError(ERROR_SUCCESS); return 0; }
    return next->first;
}
HANDLE WINAPI clipboard_data(UINT format)
{
    std::lock_guard lock(clipboard_mutex);
    auto found = clipboard.find(format);
    if (found == clipboard.end()) return nullptr;
    if (!found->second && window_proc && owner == private_window())
        window_proc(owner, WM_RENDERFORMAT, format, 0);
    found = clipboard.find(format);
    return found == clipboard.end() ? nullptr : found->second;
}
HANDLE WINAPI set_clipboard_data(UINT format, HANDLE value)
{
    std::lock_guard lock(clipboard_mutex);
    const auto old = clipboard.find(format);
    if (old != clipboard.end() && old->second && old->second != value) GlobalFree(old->second);
    clipboard[format] = value;
    ++clipboard_sequence;
    SetLastError(ERROR_SUCCESS);
    return value;
}
BOOL WINAPI clipboard_format_available(UINT format) { return clipboard.contains(format) ? TRUE : FALSE; }
int WINAPI clipboard_format_name(UINT format, LPWSTR value, int capacity)
{
    std::lock_guard lock(clipboard_mutex);
    const auto found = format_names.find(format);
    if (found == format_names.end() || capacity <= 0 ||
        found->second.size() >= static_cast<std::size_t>(capacity)) return 0;
    std::memcpy(value, found->second.c_str(), (found->second.size() + 1) * sizeof(wchar_t));
    return static_cast<int>(found->second.size());
}
HANDLE WINAPI duplicate_data(HANDLE value, CLIPFORMAT format, UINT flags)
{
    if (duplicate_failure) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }
    if (format == CF_BITMAP || format == CF_PALETTE || format == CF_METAFILEPICT)
    {
        ++native_media_copies;
        // GDI fixtures are independent HGLOBAL bytes; no system GDI objects or clipboard are used.
        return OleDuplicateData(value, CF_DIB, flags);
    }
    return OleDuplicateData(value, format, flags);
}
int WINAPI native_get_object(HANDLE, int capacity, LPVOID out)
{
    if (!out || capacity < static_cast<int>(sizeof(native_bitmap))) return 0;
    std::memcpy(out, &native_bitmap, sizeof(native_bitmap));
    return static_cast<int>(sizeof(native_bitmap));
}
UINT WINAPI native_get_palette_entries(HPALETTE, UINT start, UINT count, LPPALETTEENTRY out)
{ return start == 0 && count == 0 && !out ? native_palette_entries : 0; }
UINT WINAPI native_get_enhanced_metafile_bits(HENHMETAFILE, UINT count, LPBYTE out)
{ return count == 0 && !out ? native_enhanced_metafile_bytes : 0; }
UINT WINAPI native_get_metafile_bits(HMETAFILE, UINT count, LPVOID out)
{ return count == 0 && !out ? native_metafile_bytes : 0; }
HENHMETAFILE WINAPI native_copy_enhanced_metafile(HENHMETAFILE source, LPCWSTR)
{
    ++native_media_copies;
    return static_cast<HENHMETAFILE>(OleDuplicateData(source, CF_DIB, 0));
}
BOOL WINAPI native_delete_object(HGDIOBJ object) { return GlobalFree(object) == nullptr; }
BOOL WINAPI native_delete_enhanced_metafile(HENHMETAFILE object) { return GlobalFree(object) == nullptr; }
BOOL WINAPI native_delete_metafile(HMETAFILE) { return TRUE; }
ULONGLONG WINAPI ticks() { return tick; }
BOOL WINAPI peek_message(LPMSG, HWND, UINT, UINT, UINT)
{
    tick += 10;
    if (reader_waits != 0)
    {
        if (--reader_waits == 0) close_clipboard();
        return FALSE;
    }
    if (!paste_pending || paste_mode == PasteMode::Timeout) return FALSE;
    paste_pending = false;
    if (paste_mode == PasteMode::UserCopy)
    {
        if (open_clipboard(reinterpret_cast<HWND>(700)))
        {
            empty_clipboard();
            set_clipboard_data(CF_UNICODETEXT, text_data(L"new user clipboard"));
            close_clipboard();
        }
        return FALSE;
    }
    const HWND reader = paste_mode == PasteMode::UnknownReader ? nullptr :
        paste_mode == PasteMode::ForeignReader ? reinterpret_cast<HWND>(700) : reinterpret_cast<HWND>(100);
    if (open_clipboard(reader))
    {
        const HANDLE memory = clipboard_data(CF_UNICODETEXT);
        if (memory) consumed_text = clipboard_text();
        if (paste_mode == PasteMode::ReaderKeepsLock) reader_waits = 3;
        else close_clipboard();
    }
    return FALSE;
}
LRESULT WINAPI dispatch_message(const MSG *) { return 0; }
DWORD WINAPI wait_messages(DWORD, const HANDLE *, DWORD, DWORD, DWORD)
{
    if (ole_flush_blocked.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tick += 25; return WAIT_TIMEOUT;
}
void reset()
{
    std::lock_guard lock(clipboard_mutex);
    foreground = 100; pid = 42; foreground_calls = 0; switch_at_call = 0;
    focused_child = 0;
    sends = 0; event_count = 0;
    sent_events.clear();
    process_image.clear();
    free_clipboard();
    format_names = {{0xc010, L"HTML Format"}};
    window_proc = nullptr; window_data = 0;
    owner = reinterpret_cast<HWND>(800); opener = nullptr; clipboard_open = false;
    clipboard_sequence = 7; empties = 0; clipboard_opens = 0;
    duplicate_failure = clipboard_unavailable = modifier_held = paste_pending = false;
    accepted_events = 4; tick = 1000; consumed_text.clear();
    paste_mode = PasteMode::Consume; reader_waits = 0; restored_before_reader_closed = false;
    ole_current.Reset(); ole_streams.clear();
    ole_gets = 0; ole_sets = 0; ole_flushes = 0; ole_data_calls = 0;
    ole_source_stream_position = ~0ULL;
    ole_get_blocked = ole_get_release = ole_get_waiting = false;
    ole_get_failure = ole_unknown_medium = ole_change_during_capture = false;
    ole_flush_blocked = false;
    oversized_medium = false;
    native_bitmap = {};
    native_bitmap.bmWidth = native_bitmap.bmHeight = 1;
    native_bitmap.bmWidthBytes = 4;
    native_bitmap.bmPlanes = 1;
    native_bitmap.bmBitsPixel = 32;
    native_palette_entries = 3;
    native_enhanced_metafile_bytes = native_metafile_bytes = 64;
    native_media_copies = 0;
}
}

#define GetForegroundWindow fake_input::foreground_window
#define GetWindowThreadProcessId fake_input::window_process
#define SendInput(...) fake_input::send(__VA_ARGS__)
#define CoCreateInstance fake_input::create_instance
#define OpenProcess fake_input::open_process
#define CloseHandle fake_input::close_handle
#define QueryFullProcessImageNameW fake_input::process_image_name
#define GetWindowTextW fake_input::title
#define GetAsyncKeyState fake_input::key_state
#define GetGUIThreadInfo fake_input::gui_thread_info
#define IsWindow fake_input::is_window
#define IsWindowUnicode fake_input::is_unicode_window
#define GetClassNameW fake_input::class_name
#define RegisterClassW fake_input::register_class
#define RegisterClassExW fake_input::register_class_ex
#define UnregisterClassW fake_input::unregister_class
#define CreateWindowExW fake_input::create_window
#define DestroyWindow fake_input::destroy_window
#define SetWindowLongPtrW fake_input::set_window_data
#define GetWindowLongPtrW fake_input::get_window_data
#define DefWindowProcW fake_input::default_window_proc
#define OpenClipboard fake_input::open_clipboard
#define CloseClipboard fake_input::close_clipboard
#define EmptyClipboard fake_input::empty_clipboard
#define EnumClipboardFormats fake_input::clipboard_formats
#define GetClipboardData fake_input::clipboard_data
#define SetClipboardData fake_input::set_clipboard_data
#define GetClipboardOwner fake_input::clipboard_owner
#define GetOpenClipboardWindow fake_input::clipboard_opener
#define GetClipboardSequenceNumber fake_input::clipboard_number
#define IsClipboardFormatAvailable fake_input::clipboard_format_available
#define GetClipboardFormatNameW fake_input::clipboard_format_name
#define OleDuplicateData fake_input::duplicate_data
#define GetObjectW fake_input::native_get_object
#define GetPaletteEntries fake_input::native_get_palette_entries
#define GetEnhMetaFileBits fake_input::native_get_enhanced_metafile_bits
#define GetMetaFileBitsEx fake_input::native_get_metafile_bits
#define CopyEnhMetaFileW fake_input::native_copy_enhanced_metafile
#define DeleteObject fake_input::native_delete_object
#define DeleteEnhMetaFile fake_input::native_delete_enhanced_metafile
#define DeleteMetaFile fake_input::native_delete_metafile
#define GlobalSize fake_input::global_size
#define OleInitialize fake_input::ole_initialize
#define OleUninitialize fake_input::ole_uninitialize
#define OleGetClipboard fake_input::ole_get_clipboard
#define OleSetClipboard fake_input::ole_set_clipboard
#define OleFlushClipboard fake_input::ole_flush_clipboard
#define OleIsCurrentClipboard fake_input::ole_is_current
#define CoEnableCallCancellation fake_input::enable_cancellation
#define CoDisableCallCancellation fake_input::disable_cancellation
#define CoCancelCall fake_input::cancel_call
#define GetTickCount64 fake_input::ticks
#define PeekMessageW fake_input::peek_message
#define DispatchMessageW fake_input::dispatch_message
#define MsgWaitForMultipleObjectsEx fake_input::wait_messages
#include "../src/action/ui_input.cpp"

namespace fake_input
{
class OleSource final : public IDataObject
{
public:
    explicit OleSource(std::shared_ptr<const hmi::action::detail::ClipboardSnapshot> snapshot)
    { data_.Attach(new hmi::action::detail::ClipboardDataObject(std::move(snapshot))); ++ole_live_sources; }
    ~OleSource()
    {
        if (returned_stream_)
        {
            LARGE_INTEGER zero{};
            ULARGE_INTEGER position{};
            if (SUCCEEDED(returned_stream_->Seek(zero, STREAM_SEEK_CUR, &position)))
                ole_source_stream_position.store(position.QuadPart);
        }
        --ole_live_sources;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDataObject) return E_NOINTERFACE;
        *out = static_cast<IDataObject *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG left = --references_; if (!left) delete this; return left; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *out) override
    {
        ++ole_data_calls;
        if (ole_get_blocked.load())
        {
            ole_get_waiting.store(true);
            while (!ole_get_release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (ole_get_failure.load()) return E_FAIL;
        if (ole_unknown_medium.load()) { *out = {}; return S_OK; }
        if (ole_change_during_capture.exchange(false))
        {
            std::lock_guard lock(clipboard_mutex);
            if (open_clipboard(reinterpret_cast<HWND>(700)))
            {
                empty_clipboard();
                set_clipboard_data(CF_UNICODETEXT, text_data(L"new user clipboard"));
                close_clipboard();
            }
        }
        const HRESULT status = data_->GetData(format, out);
        if (SUCCEEDED(status) && out->tymed == TYMED_ISTREAM && out->pstm)
        {
            LARGE_INTEGER initial{};
            initial.QuadPart = 3;
            (void)out->pstm->Seek(initial, STREAM_SEEK_SET, nullptr);
            returned_stream_ = out->pstm;
        }
        return status;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *f, STGMEDIUM *m) override { return data_->GetDataHere(f, m); }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *f) override { return data_->QueryGetData(f); }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *f, FORMATETC *out) override
    { return data_->GetCanonicalFormatEtc(f, out); }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *f, STGMEDIUM *m, BOOL release) override { return data_->SetData(f, m, release); }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override
    { return data_->EnumFormatEtc(direction, out); }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }
private:
    std::atomic<ULONG> references_{1};
    Microsoft::WRL::ComPtr<IDataObject> data_;
    Microsoft::WRL::ComPtr<IStream> returned_stream_;
};

HRESULT WINAPI ole_get_clipboard(IDataObject **out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    ++ole_gets;
    std::lock_guard lock(clipboard_mutex);
    auto snapshot = std::make_shared<hmi::action::detail::ClipboardSnapshot>();
    for (const auto &[format, memory] : clipboard)
    {
        if (hmi::action::detail::ole_transport_format(format)) continue;
        hmi::action::detail::ClipboardValue value;
        value.format = {static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        const auto stream = ole_streams.find(format);
        if (stream != ole_streams.end())
        {
            value.format.tymed = TYMED_ISTREAM;
            value.stream = stream->second;
            value.bytes = value.stream.size();
        }
        else
        {
            value.data = duplicate_data(memory, static_cast<CLIPFORMAT>(format), 0);
            value.bytes = GlobalSize(memory);
            if (!value.data) return E_FAIL;
        }
        snapshot->values.push_back(std::move(value));
    }
    *out = new OleSource(std::move(snapshot));
    return S_OK;
}

HRESULT WINAPI ole_set_clipboard(IDataObject *object)
{
    std::lock_guard lock(clipboard_mutex);
    if (!object || !open_clipboard(reinterpret_cast<HWND>(1000))) return CLIPBRD_E_CANT_OPEN;
    empty_clipboard();
    ole_current = object;
    ++ole_sets;
    // Transport is regenerated, never copied from the old private handle.
    format_names[ole_marker] = L"DataObject";
    set_clipboard_data(ole_marker, binary_data({'O', 'L', 'E', 0}));
    close_clipboard();
    return S_OK;
}

HRESULT WINAPI ole_is_current(IDataObject *object)
{
    std::lock_guard lock(clipboard_mutex);
    return ole_current.Get() == object ? S_OK : S_FALSE;
}

HRESULT WINAPI ole_flush_clipboard()
{
    ++ole_flushes;
    if (ole_flush_blocked.load()) return CLIPBRD_E_CANT_OPEN;
    std::lock_guard lock(clipboard_mutex);
    if (!ole_current) return E_FAIL;
    Microsoft::WRL::ComPtr<IEnumFORMATETC> formats;
    if (FAILED(ole_current->EnumFormatEtc(DATADIR_GET, &formats))) return E_FAIL;
    FORMATETC format{};
    ULONG fetched = 0;
    while (formats->Next(1, &format, &fetched) == S_OK)
    {
        STGMEDIUM medium{};
        if (FAILED(ole_current->GetData(&format, &medium))) return E_FAIL;
        if (medium.tymed == TYMED_HGLOBAL)
        {
            set_clipboard_data(format.cfFormat, medium.hGlobal);
            medium.hGlobal = nullptr;
            medium.tymed = TYMED_NULL;
        }
        else if (medium.tymed == TYMED_ISTREAM)
        {
            STATSTG info{};
            if (FAILED(medium.pstm->Stat(&info, STATFLAG_NONAME)) || info.cbSize.QuadPart > 32 * 1024 * 1024)
            { ReleaseStgMedium(&medium); return E_FAIL; }
            std::vector<unsigned char> bytes(static_cast<std::size_t>(info.cbSize.QuadPart));
            ULONG read = 0;
            const HRESULT status = medium.pstm->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
            if (FAILED(status) || read != bytes.size()) { ReleaseStgMedium(&medium); return E_FAIL; }
            set_clipboard_data(format.cfFormat, binary_data(bytes));
        }
        else { ReleaseStgMedium(&medium); return E_FAIL; }
        ReleaseStgMedium(&medium);
    }
    ole_current.Reset();
    return S_OK;
}

bool wait_ole(const std::function<bool()> &condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void prepare_ole()
{
    process_image = L"C:\\Tencent\\Weixin.exe";
    format_names[ole_marker] = L"DataObject";
    clipboard[CF_UNICODETEXT] = text_data(L"saved OLE clipboard");
    clipboard[ole_marker] = binary_data({0x44, 0, 0xff, 0});
}
}

int main()
{
    using namespace hmi::action;
    int failures = 0;
    auto check = [&](bool value, const char *name) {
        std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
        if (!value) ++failures;
    };
    Target target;
    target.pid = 42;
    target.native_window = 100;
    TextInput input;
    fake_input::reset();
    fake_input::foreground = 200;
    const auto refused = input.insert("hello", target, "sendinput");
    check(!refused.ok && fake_input::sends == 0,
          "release-time window is never rebound to a new foreground window");
    check(refused.total_ms >= 0.0 && !refused.uia_attempted && refused.uia_ms == -1.0 &&
          refused.uia_worker_ms == -1.0 && !refused.send_input_attempted && refused.send_input_ms == -1.0 &&
          !refused.clipboard_attempted && refused.clipboard_ms == -1.0 && refused.send_input_calls == 0,
          "refused input measures total elapsed time and leaves unused stages unknown");
    fake_input::reset();
    fake_input::pid = 43;
    check(!input.insert("hello", target, "sendinput").ok && fake_input::sends == 0,
          "reused window with changed PID is rejected");
    fake_input::reset();
    fake_input::switch_at_call = 3;
    check(!input.insert("hello", target, "sendinput").ok && fake_input::sends == 0,
          "window switch during event preparation is rejected before SendInput");
    fake_input::reset();
    const auto direct = input.insert("hello", target, "sendinput");
    check(direct.ok && direct.path == InputPath::SendInput && fake_input::sends == 1 && fake_input::event_count == 10,
          "unchanged explicit target receives one complete Unicode event batch");
    check(direct.total_ms >= direct.send_input_ms && direct.send_input_ms >= 0.0 &&
          direct.send_input_attempted && direct.send_input_calls == 1 &&
          !direct.uia_attempted && direct.uia_ms == -1.0 && direct.uia_worker_ms == -1.0 &&
          !direct.clipboard_attempted && direct.clipboard_ms == -1.0,
          "direct Unicode input measures only the API stage actually used");
    check(direct.delivery == InputDelivery::DeliveredUnverified && direct.verification_after_ms == -1.0,
          "API acceptance without an available observer remains delivered_unverified with unknown after-read timing");
    fake_input::reset();
    fake_input::focused_child = 102;
    Target changed_child = target;
    changed_child.native_focus = 101;
    const auto wrong_child = input.insert("hello", changed_child, "sendinput");
    check(!wrong_child.ok && wrong_child.delivery == InputDelivery::NotSent && fake_input::sends == 0,
          "same top-level window with another focused child cannot receive stale text");
    fake_input::reset();
    fake_input::modifier_held = true;
    const auto modified = input.insert("hello", target, "sendinput");
    check(!modified.ok && fake_input::sends == 0 && modified.delivery == InputDelivery::NotSent,
          "held modifiers refuse generic Unicode input instead of turning it into a shortcut");
    fake_input::reset();
    const auto chinese = input.insert("中文🙂", target, "sendinput");
    const std::wstring expected = L"中文\U0001f642";
    bool unicode_events = fake_input::sent_events.size() == expected.size() * 2;
    for (std::size_t index = 0; unicode_events && index < expected.size(); ++index)
    {
        const auto &down = fake_input::sent_events[index * 2];
        const auto &up = fake_input::sent_events[index * 2 + 1];
        unicode_events = down.type == INPUT_KEYBOARD && up.type == INPUT_KEYBOARD &&
            down.ki.wVk == 0 && up.ki.wVk == 0 && down.ki.wScan == expected[index] &&
            up.ki.wScan == expected[index] && down.ki.dwFlags == KEYEVENTF_UNICODE &&
            up.ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
    }
    check(chinese.ok && fake_input::sends == 1 && unicode_events,
          "Chinese and supplementary Unicode use paired text events without a submit key");
    fake_input::reset();
    const std::string question = "明天晚上会不会下雨？";
    const std::wstring question_wide = L"明天晚上会不会下雨？";
    const auto unicode_question = input.insert(question, target, "sendinput");
    check(unicode_question.ok && fake_input::sent_events.size() == question_wide.size() * 2 &&
          fake_input::sent_events.front().ki.wScan == 0x660e,
          "generic Unicode transport preserves the reported sentence first codepoint U+660E");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"original clipboard");
    fake_input::clipboard[0xc010] = fake_input::text_data(L"<b>original rich text</b>");
    const auto wechat = input.insert(question, target, "sendinput");
    bool paste_events = fake_input::sent_events.size() == 4;
    constexpr WORD paste_keys[] = {VK_CONTROL, 'V', 'V', VK_CONTROL};
    for (std::size_t index = 0; paste_events && index < 4; ++index)
    {
        const INPUT &event = fake_input::sent_events[index];
        paste_events = event.type == INPUT_KEYBOARD && event.ki.wVk == paste_keys[index] &&
            event.ki.wScan == 0 && event.ki.dwFlags == (index < 2 ? 0u : KEYEVENTF_KEYUP);
    }
    check(wechat.ok && wechat.path == InputPath::SendInput && fake_input::sends == 1 && paste_events &&
          fake_input::consumed_text == question_wide && fake_input::consumed_text.front() == 0x660e,
          "Weixin consumes the complete Unicode sentence through one Ctrl+V without Enter");
    check(fake_input::clipboard_text() == L"original clipboard" &&
          fake_input::clipboard_text(0xc010) == L"<b>original rich text</b>",
          "confirmed target read restores every snapshotted text and HTML format");
    check(wechat.total_ms >= wechat.clipboard_ms && wechat.clipboard_ms >= wechat.send_input_ms &&
          wechat.send_input_ms >= 0.0 && wechat.clipboard_attempted && wechat.send_input_attempted &&
          wechat.send_input_calls == 1 && !wechat.uia_attempted && wechat.uia_ms == -1.0,
          "clipboard transaction and SendInput timings cover the same successful paste without a UIA attempt");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    constexpr UINT chromium_token = 49753;
    constexpr UINT chromium_url = 49641;
    fake_input::format_names[chromium_token] = L"Chromium internal source RFH token";
    fake_input::format_names[chromium_url] = L"Chromium internal source URL";
    fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"original browser clipboard");
    fake_input::clipboard[0xc010] = fake_input::text_data(L"<b>original browser text</b>");
    fake_input::clipboard[chromium_token] = fake_input::binary_data({0x18, 0, 0, 0, 0x71, 0, 0x81, 0xff});
    fake_input::clipboard[chromium_url] = fake_input::binary_data({'h', 't', 't', 'p', ':', '/', '/', 'x', 0});
    fake_input::clipboard[CF_LOCALE] = fake_input::binary_data({0x04, 0x08, 0, 0});
    fake_input::clipboard[CF_TEXT] = fake_input::binary_data({'o', 'l', 'd', 0});
    fake_input::clipboard[CF_OEMTEXT] = fake_input::binary_data({'o', 'l', 'd', 0});
    std::map<UINT, std::vector<unsigned char>> browser_snapshot;
    for (const auto &[format, data] : fake_input::clipboard)
    {
        (void)data;
        browser_snapshot[format] = fake_input::clipboard_bytes(format);
    }
    const auto browser_clipboard = input.insert(question, target, "sendinput");
    bool browser_restored = fake_input::clipboard.size() == browser_snapshot.size();
    for (const auto &[format, bytes] : browser_snapshot)
        browser_restored = browser_restored && fake_input::clipboard_bytes(format) == bytes;
    check(browser_clipboard.ok && fake_input::sends == 1 && fake_input::sent_events.size() == 4 &&
          fake_input::consumed_text == question_wide && browser_restored,
          "Chromium source metadata no longer blocks one complete paste and every original format is restored byte-for-byte");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    constexpr UINT custom_format = 0xc100;
    fake_input::format_names[custom_format] = L"HMI custom MIME packet";
    fake_input::clipboard[custom_format] = fake_input::binary_data({0, 0xff, 'm', 0, 0x80, 0x7f});
    const auto custom_before = fake_input::clipboard_bytes(custom_format);
    const auto custom_clipboard = input.insert(question, target, "sendinput");
    check(custom_clipboard.ok && fake_input::sends == 1 && fake_input::consumed_text == question_wide &&
          fake_input::clipboard_bytes(custom_format) == custom_before,
          "an ordinary unknown registered HGLOBAL is copied as opaque bytes instead of rejected by its name");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    constexpr UINT history_exclude = 0xc110;
    constexpr UINT history_include = 0xc111;
    constexpr UINT cloud_upload = 0xc112;
    fake_input::format_names[history_exclude] = L"ExcludeClipboardContentFromMonitorProcessing";
    fake_input::format_names[history_include] = L"CanIncludeInClipboardHistory";
    fake_input::format_names[cloud_upload] = L"CanUploadToCloudClipboard";
    fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved cloud preferences");
    fake_input::clipboard[history_exclude] = fake_input::binary_data({0, 0, 0, 0});
    fake_input::clipboard[history_include] = fake_input::binary_data({1, 0, 0, 0});
    fake_input::clipboard[cloud_upload] = fake_input::binary_data({0, 0, 0, 0});
    const auto exclude_before = fake_input::clipboard_bytes(history_exclude);
    const auto include_before = fake_input::clipboard_bytes(history_include);
    const auto cloud_before = fake_input::clipboard_bytes(cloud_upload);
    const auto cloud_clipboard = input.insert(question, target, "sendinput");
    check(cloud_clipboard.ok && fake_input::sends == 1 && fake_input::consumed_text == question_wide &&
          fake_input::clipboard_text() == L"saved cloud preferences" &&
          fake_input::clipboard_bytes(history_exclude) == exclude_before &&
          fake_input::clipboard_bytes(history_include) == include_before &&
          fake_input::clipboard_bytes(cloud_upload) == cloud_before,
          "Windows clipboard history and cloud DWORD metadata survive the paste unchanged");

    constexpr UINT native_media_formats[] = {CF_BITMAP, CF_DSPBITMAP, CF_PALETTE, CF_ENHMETAFILE,
        CF_DSPENHMETAFILE, CF_METAFILEPICT, CF_DSPMETAFILEPICT};
    constexpr UINT maximum_snapshot_bytes = 32U * 1024 * 1024;
    for (const UINT format : native_media_formats)
    {
        fake_input::reset();
        fake_input::process_image = L"C:\\Editors\\Notepad.exe";
        fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved native media clipboard");
        fake_input::clipboard[format] = fake_input::native_media_data(format);
        const auto original = fake_input::clipboard_bytes(format);
        const auto preserved = input.insert(question, target, "sendinput");
        check(preserved.ok && fake_input::sends == 1 && fake_input::native_media_copies == 1 &&
              fake_input::clipboard_text() == L"saved native media clipboard" &&
              fake_input::clipboard_bytes(format) == original,
              "bounded bitmap, palette and metafile payloads survive native clipboard restoration");

        fake_input::reset();
        fake_input::process_image = L"C:\\Editors\\Notepad.exe";
        fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved native media clipboard");
        fake_input::clipboard[format] = fake_input::native_media_data(format);
        const HANDLE original_handle = fake_input::clipboard[format];
        const auto oversized_original = fake_input::clipboard_bytes(format);
        fake_input::native_bitmap.bmHeight = fake_input::native_bitmap.bmWidthBytes = MAXLONG;
        fake_input::native_bitmap.bmPlanes = 0xffff;
        fake_input::native_palette_entries = 0xffffffffU;
        fake_input::native_enhanced_metafile_bytes = maximum_snapshot_bytes + 1;
        fake_input::native_metafile_bytes = maximum_snapshot_bytes;
        const auto oversized = input.insert(question, target, "sendinput");
        check(!oversized.ok && fake_input::sends == 0 && fake_input::empties == 0 &&
              fake_input::native_media_copies == 0 && fake_input::clipboard[format] == original_handle &&
              fake_input::clipboard_text() == L"saved native media clipboard" &&
              fake_input::clipboard_bytes(format) == oversized_original &&
              oversized.detail.find("size_limit") != std::string::npos,
              "native media beyond 32 MiB, including bitmap overflow and metafile payload, is rejected before copying or input");

        fake_input::reset();
        fake_input::process_image = L"C:\\Editors\\Notepad.exe";
        fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved native media clipboard");
        fake_input::clipboard[format] = fake_input::native_media_data(format);
        fake_input::native_bitmap.bmHeight = 0;
        fake_input::native_palette_entries = 0;
        fake_input::native_enhanced_metafile_bytes = fake_input::native_metafile_bytes = 0;
        const auto invalid_size = input.insert(question, target, "sendinput");
        check(!invalid_size.ok && fake_input::sends == 0 && fake_input::empties == 0 &&
              fake_input::native_media_copies == 0 && fake_input::clipboard_text() == L"saved native media clipboard",
              "zero native media dimensions or payload sizes leave the original clipboard untouched");
    }
    for (const bool exceeds_total : {false, true})
    {
        fake_input::reset();
        fake_input::process_image = L"C:\\Editors\\Notepad.exe";
        fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved combined budget");
        fake_input::clipboard[CF_ENHMETAFILE] = fake_input::native_media_data(CF_ENHMETAFILE);
        const auto text_bytes = GlobalSize(fake_input::clipboard[CF_UNICODETEXT]);
        fake_input::native_enhanced_metafile_bytes = maximum_snapshot_bytes - static_cast<UINT>(text_bytes) +
            (exceeds_total ? 1U : 0U);
        const auto bounded_total = input.insert(question, target, "sendinput");
        check(bounded_total.ok == !exceeds_total && fake_input::sends == (exceeds_total ? 0U : 1U) &&
              fake_input::native_media_copies == (exceeds_total ? 0U : 1U) &&
              (!exceeds_total || fake_input::empties == 0) &&
              fake_input::clipboard_text() == L"saved combined budget",
              "native text and metafile share one budget: exactly 32 MiB succeeds and one extra byte fails before media copying");
    }

    constexpr const wchar_t *owner_formats[] = {L"DataObject", L"Ole Private Data"};
    for (const auto &owner_format : owner_formats)
    {
        fake_input::reset();
        fake_input::prepare_ole();
        constexpr UINT owner_format_id = 0xc200;
        fake_input::format_names[owner_format_id] = owner_format;
        constexpr UINT stream_format = 0xc201;
        const std::vector<unsigned char> original_stream{0, 0xff, 's', 't', 'r', 'e', 'a', 'm', 0};
        fake_input::format_names[stream_format] = L"Fixture stream";
        fake_input::clipboard[stream_format] = fake_input::binary_data(original_stream);
        fake_input::ole_streams[stream_format] = original_stream;
        const auto owner_before = fake_input::clipboard_bytes(owner_format_id);
        const auto owner_result = input.insert(question, target, "sendinput");
        check(owner_result.ok && fake_input::sends == 1 && fake_input::ole_gets == 1 &&
              fake_input::ole_sets == 1 && fake_input::ole_flushes == 1 &&
              fake_input::clipboard_text() == L"saved OLE clipboard" &&
              fake_input::clipboard_bytes(stream_format) == original_stream &&
              fake_input::clipboard_bytes(owner_format_id) != owner_before && fake_input::ole_source_stream_position == 3,
              "OLE text and stream payloads are frozen and restored through regenerated transport and Flush, never private handle copies");
    }

    for (const bool unknown_medium : {false, true})
    {
        fake_input::reset();
        fake_input::prepare_ole();
        fake_input::ole_get_failure = !unknown_medium;
        fake_input::ole_unknown_medium = unknown_medium;
        const auto original = fake_input::clipboard_bytes(fake_input::ole_marker);
        const auto rejected = input.insert(question, target, "sendinput");
        check(!rejected.ok && fake_input::sends == 0 && fake_input::empties == 0 && fake_input::ole_sets == 0 &&
              fake_input::clipboard_text() == L"saved OLE clipboard" &&
              fake_input::clipboard_bytes(fake_input::ole_marker) == original,
              "GetData errors and unsupported media leave every old clipboard format untouched before any input");
    }
    fake_input::reset();
    fake_input::prepare_ole();
    const auto no_owner = insert_text(question);
    check(!no_owner.ok && fake_input::sends == 0 && fake_input::empties == 0 && fake_input::ole_gets == 0,
          "the compatibility API without a persistent job owner refuses OLE before changing anything");
    fake_input::reset();
    fake_input::prepare_ole();
    for (UINT index = 0; index < 64; ++index)
    {
        const UINT format = 0xc300 + index;
        fake_input::format_names[format] = L"Fixture public format";
        fake_input::clipboard[format] = fake_input::binary_data({1});
    }
    const auto too_many = input.insert(question, target, "sendinput");
    check(!too_many.ok && fake_input::sends == 0 && fake_input::empties == 0 && fake_input::ole_gets == 0,
          "more than 64 native formats fail before OLE calls, clearing or input");
    fake_input::reset();
    fake_input::prepare_ole();
    fake_input::oversized_medium = true;
    const auto too_large = input.insert(question, target, "sendinput");
    check(!too_large.ok && fake_input::sends == 0 && fake_input::empties == 0 && fake_input::ole_sets == 0 &&
          fake_input::clipboard_text() == L"saved OLE clipboard" && too_large.detail.find("size_limit") != std::string::npos,
          "a medium larger than 32 MiB is refused without clearing the original or sending input");
    fake_input::reset();
    fake_input::prepare_ole();
    fake_input::ole_change_during_capture = true;
    const auto changed_snapshot = input.insert(question, target, "sendinput");
    check(!changed_snapshot.ok && fake_input::sends == 0 && fake_input::ole_sets == 0 &&
          fake_input::clipboard_text() == L"new user clipboard",
          "a user's copy during OLE materialization cancels delivery without restoring stale content");

    fake_input::reset();
    fake_input::prepare_ole();
    fake_input::ole_get_blocked = true;
    {
        TextInput blocked_input;
        const auto started = std::chrono::steady_clock::now();
        const auto timed_out = blocked_input.insert(question, target, "sendinput");
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto pending = blocked_input.insert(question, target, "sendinput");
        check(!timed_out.ok && !pending.ok && fake_input::ole_get_waiting && fake_input::ole_gets == 1 &&
              fake_input::sends == 0 && fake_input::empties == 0 && elapsed < std::chrono::seconds(2),
              "uncancellable GetData has a bounded caller wait and one owned job, without late input or clipboard writes");
        fake_input::ole_get_release = true;
        check(fake_input::wait_ole([] { return fake_input::ole_live_sources == 0; }) && fake_input::sends == 0 &&
              fake_input::empties == 0, "late cancelled OLE capture releases its media and cannot publish or paste");
    }

    for (const bool new_copy : {false, true})
    {
        fake_input::reset();
        fake_input::prepare_ole();
        detail::ClipboardSnapshotWorker snapshots;
        std::shared_ptr<const detail::ClipboardSnapshot> snapshot;
        std::string error;
        check(snapshots.capture({CF_UNICODETEXT, fake_input::ole_marker}, snapshot, error),
              "restore lifecycle fixture captured independent original content");
        fake_input::ole_flush_blocked = true;
        const auto pending = snapshots.restore(snapshot, fake_input::owner, fake_input::clipboard_sequence, error);
        check(pending == detail::ClipboardRestoreResult::Pending && snapshots.busy() && fake_input::ole_sets == 1 &&
              fake_input::ole_current.Get() != nullptr,
              "failed Flush retains its self-owned STA source and returns an explicit pending outcome");
        if (new_copy)
        {
            std::lock_guard lock(fake_input::clipboard_mutex);
            fake_input::open_clipboard(reinterpret_cast<HWND>(700));
            fake_input::empty_clipboard();
            fake_input::set_clipboard_data(CF_UNICODETEXT, fake_input::text_data(L"new user clipboard"));
            fake_input::close_clipboard();
        }
        fake_input::ole_flush_blocked = false;
        check(fake_input::wait_ole([&] { return !snapshots.busy(); }) && fake_input::ole_sets == 1 &&
              fake_input::clipboard_text() == (new_copy ? L"new user clipboard" : L"saved OLE clipboard"),
              "pending restoration only flushes its own object, never republishes or overwrites a later user copy");
    }

    for (const auto *process : {L"Notepad.exe", L"wordpad.exe", L"WINWORD.EXE", L"EXCEL.EXE"})
    {
        fake_input::reset();
        fake_input::process_image = std::wstring(L"C:\\Editors\\") + process;
        fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved editor clipboard");
        const auto result = input.insert(question, target, "sendinput");
        check(result.ok && fake_input::sends == 1 && fake_input::sent_events.size() == 4 &&
              fake_input::consumed_text == question_wide && fake_input::clipboard_text() == L"saved editor clipboard" &&
              result.detail.find("wechat") == std::string::npos && result.delivery == InputDelivery::DeliveredUnverified,
              "exact editor process basenames use one caret paste and preserve the user's clipboard without claiming visual verification");
    }

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\wEcHaT.ExE";
    fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved");
    fake_input::paste_mode = fake_input::PasteMode::ReaderKeepsLock;
    const auto reader_locked = input.insert(question, target, "sendinput");
    check(reader_locked.ok && fake_input::consumed_text == question_wide &&
          !fake_input::restored_before_reader_closed && fake_input::clipboard_text() == L"saved",
          "case-insensitive WeChat basename waits for the target to close the clipboard before restore");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe.backup";
    const auto unrelated = input.insert(question, target, "sendinput");
    check(unrelated.ok && fake_input::clipboard_opens == 0 &&
          fake_input::sent_events.size() == question_wide.size() * 2,
          "similarly named processes keep normal Unicode transport without accessing the clipboard");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved");
    fake_input::paste_mode = fake_input::PasteMode::UserCopy;
    const auto user_copy = input.insert(question, target, "sendinput");
    check(!user_copy.ok && fake_input::sends == 1 &&
          fake_input::clipboard_text() == L"new user clipboard",
          "a newer clipboard owner and sequence are preserved without repeating input");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved");
    fake_input::paste_mode = fake_input::PasteMode::Timeout;
    const auto timeout = input.insert(question, target, "sendinput");
    check(!timeout.ok && fake_input::sends == 1 && fake_input::consumed_text.empty() &&
          fake_input::clipboard_text() == question_wide,
          "unconfirmed paste keeps rendered Unicode available and never pastes old clipboard text");
    check(timeout.total_ms >= timeout.clipboard_ms && timeout.clipboard_ms >= 0.0 &&
          timeout.send_input_attempted && timeout.send_input_calls == 1 && !timeout.uia_timed_out,
          "unconfirmed clipboard outcome retains elapsed measurements independently of UIA timeout state");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::paste_mode = fake_input::PasteMode::UnknownReader;
    const auto anonymous_reader = input.insert(question, target, "sendinput");
    check(!anonymous_reader.ok && fake_input::sends == 1 && fake_input::clipboard_text() == question_wide,
          "a clipboard reader without an HWND cannot be mistaken for a confirmed WeChat paste");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::paste_mode = fake_input::PasteMode::ForeignReader;
    const auto clipboard_manager = input.insert(question, target, "sendinput");
    check(!clipboard_manager.ok && fake_input::sends == 1 && fake_input::clipboard_text() == question_wide,
          "a foreign clipboard reader cannot cause an early restore or false paste confirmation");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved");
    fake_input::duplicate_failure = true;
    const auto duplicate_failed = input.insert(question, target, "sendinput");
    check(!duplicate_failed.ok && fake_input::sends == 0 && fake_input::empties == 0 &&
          fake_input::clipboard_text() == L"saved" &&
          duplicate_failed.detail.find("format=13") != std::string::npos &&
          duplicate_failed.detail.find("stage=duplicate") != std::string::npos &&
          duplicate_failed.detail.find("error=") != std::string::npos,
          "snapshot duplication failure leaves the clipboard and target unchanged");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::clipboard[CF_PRIVATEFIRST] = fake_input::text_data(L"private owner data");
    check(!input.insert(question, target, "sendinput").ok && fake_input::sends == 0 &&
          fake_input::empties == 0 && fake_input::clipboard_text(CF_PRIVATEFIRST) == L"private owner data",
          "unrestorable owner-specific data is rejected before clearing any clipboard format");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::modifier_held = true;
    check(!input.insert(question, target, "sendinput").ok && fake_input::sends == 0 && fake_input::empties == 0,
          "held modifiers cannot change a clipboard paste into a shortcut");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved");
    fake_input::accepted_events = 1;
    const auto rejected_paste = input.insert(question, target, "sendinput");
    check(!rejected_paste.ok && fake_input::sends == 2 && fake_input::sent_events.size() == 6 &&
          fake_input::sent_events[4].ki.dwFlags == KEYEVENTF_KEYUP &&
          fake_input::sent_events[5].ki.dwFlags == KEYEVENTF_KEYUP &&
          fake_input::clipboard_text() == L"saved",
          "partial key delivery releases V and Control without retrying the text");
    check(rejected_paste.send_input_calls == 2 && rejected_paste.send_input_ms >= 0.0,
          "SendInput observation counts the existing release-only cleanup separately");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved");
    fake_input::accepted_events = 2;
    const auto uncertain_paste = input.insert(question, target, "sendinput");
    check(!uncertain_paste.ok && fake_input::sends == 2 && fake_input::consumed_text == question_wide &&
          fake_input::clipboard_text() == L"saved",
          "possibly submitted partial paste is observed once and never replaced with Unicode retries");

    fake_input::reset();
    fake_input::process_image = L"C:\\Tencent\\Weixin.exe";
    fake_input::clipboard[CF_UNICODETEXT] = fake_input::text_data(L"saved");
    fake_input::switch_at_call = 3;
    check(!input.insert(question, target, "sendinput").ok && fake_input::sends == 0 &&
          fake_input::clipboard_text() == L"saved",
          "foreground changes while preparing WeChat paste cancel delivery without losing prior data");
    fake_input::reset();
    const auto fallback = input.insert("hello", target, "auto");
    check(fallback.ok && fallback.path == InputPath::SendInput && fake_input::sends == 1,
          "UIA lookup failure falls back once to the same target");
    check(fallback.uia_attempted && fallback.uia_ms >= 0.0 && fallback.uia_worker_ms >= 0.0 &&
          !fallback.uia_timed_out && fallback.send_input_attempted && fallback.send_input_calls == 1 &&
          !fallback.clipboard_attempted && fallback.clipboard_ms == -1.0,
          "completed UIA lookup publishes worker timing safely before measuring one fallback");
    fake_input::reset();
    fake_input::switch_at_call = 2;
    check(!input.insert("hello", target, "auto").ok && fake_input::sends == 0,
          "UIA fallback cannot follow a changed foreground window");
    fake_input::reset();
    const auto forced_uia = input.insert("hello", target, "uia");
    check(!forced_uia.ok && fake_input::sends == 0,
          "forced UIA never falls back");
    check(forced_uia.uia_attempted && forced_uia.uia_ms >= 0.0 && forced_uia.uia_worker_ms >= 0.0 &&
          !forced_uia.send_input_attempted && forced_uia.send_input_ms == -1.0 &&
          forced_uia.send_input_calls == 0 && !forced_uia.clipboard_attempted && forced_uia.clipboard_ms == -1.0,
          "a later forced UIA result cannot inherit transport metrics from the previous request");
    check(legacy_editable(ROLE_SYSTEM_TEXT, 0), "editable legacy text can use SetValue");
    check(!legacy_editable(ROLE_SYSTEM_STATICTEXT, 0), "static legacy text does not claim writability");
    check(!legacy_editable(ROLE_SYSTEM_TEXT, STATE_SYSTEM_READONLY), "read-only legacy edit uses fallback");
    check(!legacy_editable(ROLE_SYSTEM_TEXT, STATE_SYSTEM_UNAVAILABLE), "disabled legacy edit does not claim writability");
    check(definitely_not_written(E_NOTIMPL) && definitely_not_written(UIA_E_NOTSUPPORTED),
          "explicit unsupported writes permit safe fallback");
    check(!definitely_not_written(E_FAIL) && !definitely_not_written(RPC_E_TIMEOUT) && !definitely_not_written(S_OK),
          "unknown, timed-out, or successful writes cannot be retried");
    return failures == 0 ? 0 : 1;
}
