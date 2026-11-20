#include <windows.h>
#include <ole2.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace isolated_clipboard
{
std::atomic<HWINSTA> expected_station{nullptr};
std::atomic<HDESK> expected_desktop{nullptr};
std::atomic<unsigned> ole_apartments{0};
std::atomic<unsigned> completed_flushes{0};
std::wstring expected_name;
std::wstring expected_desktop_name;

bool private_station_only() noexcept
{
    const HWINSTA station = GetProcessWindowStation();
    if (!station || station != expected_station.load()) return false;
    wchar_t name[256]{};
    USEROBJECTFLAGS flags{};
    DWORD size = 0;
    return GetUserObjectInformationW(station, UOI_NAME, name, sizeof(name), &size) &&
        expected_name == name && _wcsicmp(name, L"WinSta0") != 0 &&
        GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &size) &&
        (flags.dwFlags & WSF_VISIBLE) == 0;
}

bool private_station() noexcept
{
    if (!private_station_only() || !expected_desktop.load()) return false;
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    wchar_t name[256]{};
    DWORD size = 0;
    return desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &size) &&
        expected_desktop_name == name;
}

void require_private() noexcept
{
    if (private_station()) return;
    std::fputs("FAIL isolated clipboard guard: private noninteractive station is not current\n", stderr);
    std::fflush(stderr);
    std::_Exit(1); // Never fall back to, read, clear, or publish on WinSta0.
}

// Define the wrappers before the macros, so their calls reach the real OS.
HRESULT WINAPI initialize(void *reserved)
{
    // Bind every new STA explicitly before OLE can create its internal window.
    // Do not rely on process station changes or default desktop inheritance.
    if (!private_station_only() || !expected_desktop.load() || !SetThreadDesktop(expected_desktop.load()))
    {
        std::fputs("FAIL cannot bind OLE thread to the private desktop\n", stderr);
        std::fflush(stderr);
        std::_Exit(1);
    }
    require_private();
    const HRESULT status = OleInitialize(reserved);
    if (SUCCEEDED(status)) ++ole_apartments;
    return status;
}
void WINAPI uninitialize()
{
    require_private();
    OleUninitialize();
    --ole_apartments;
}
HRESULT WINAPI get(IDataObject **object) { require_private(); return OleGetClipboard(object); }
HRESULT WINAPI set(IDataObject *object) { require_private(); return OleSetClipboard(object); }
HRESULT WINAPI flush()
{
    require_private();
    const HRESULT status = OleFlushClipboard();
    if (SUCCEEDED(status)) ++completed_flushes;
    return status;
}
BOOL WINAPI open(HWND owner) { require_private(); return OpenClipboard(owner); }
BOOL WINAPI close() { require_private(); return CloseClipboard(); }
BOOL WINAPI empty() { require_private(); return EmptyClipboard(); }
HANDLE WINAPI data(UINT format, HANDLE value) { require_private(); return SetClipboardData(format, value); }
}

#define OleInitialize isolated_clipboard::initialize
#define OleUninitialize isolated_clipboard::uninitialize
#define OleGetClipboard isolated_clipboard::get
#define OleSetClipboard isolated_clipboard::set
#define OleFlushClipboard isolated_clipboard::flush
#define OpenClipboard isolated_clipboard::open
#define CloseClipboard isolated_clipboard::close
#define EmptyClipboard isolated_clipboard::empty
#define SetClipboardData isolated_clipboard::data
#include "action/clipboard_snapshot.hpp"

namespace
{
using namespace hmi::action::detail;
using Microsoft::WRL::ComPtr;

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

class PrivateStation
{
public:
    int enter()
    {
        // No OLE initialization, test windows, clipboard calls, or worker
        // threads are permitted before both private associations succeed.
        original_station_ = GetProcessWindowStation();
        original_desktop_ = GetThreadDesktop(GetCurrentThreadId());
        if (!original_station_ || !original_desktop_) return failure("get original context");
        name_ = L"HmiOlePrivate_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        station_ = CreateWindowStationW(name_.c_str(), CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
        if (!station_) return failure("create private station");
        if (!SetProcessWindowStation(station_)) return failure("set private station");
        station_changed_ = true;
        isolated_clipboard::expected_name = name_;
        isolated_clipboard::expected_station.store(station_);
        require(isolated_clipboard::private_station_only(), "new station is not private and noninteractive");
        desktop_name_ = name_ + L"_Desktop";
        desktop_ = CreateDesktopW(desktop_name_.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop_) return failure("create private desktop");
        if (!SetThreadDesktop(desktop_)) return failure("set private desktop");
        desktop_changed_ = true;
        isolated_clipboard::expected_desktop_name = desktop_name_;
        isolated_clipboard::expected_desktop.store(desktop_);
        isolated_clipboard::require_private();
        return 0;
    }

    ~PrivateStation()
    {
        // Pending workers must never outlive isolation and accidentally observe
        // a restored interactive station. A failing teardown stays fail-closed.
        if (isolated_clipboard::ole_apartments.load() != 0) teardown_failure("OLE apartment still active");
        isolated_clipboard::expected_station.store(nullptr);
        isolated_clipboard::expected_desktop.store(nullptr);
        if (station_changed_ && !SetProcessWindowStation(original_station_)) teardown_failure("restore process station");
        // SetThreadDesktop requires a desktop in the current process station.
        if (desktop_changed_ && !SetThreadDesktop(original_desktop_)) teardown_failure("restore thread desktop");
        if (desktop_ && !CloseDesktop(desktop_)) teardown_failure("close private desktop");
        if (station_ && !CloseWindowStation(station_)) teardown_failure("close private station");
    }

private:
    int failure(const char *stage) const
    {
        const DWORD error = GetLastError();
        const bool denied = error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD;
        std::fprintf(stderr, "%s %s error=0x%08lx; no interactive clipboard access attempted\n",
            denied ? "SKIP" : "FAIL", stage, static_cast<unsigned long>(error));
        return denied ? 77 : 1;
    }
    [[noreturn]] void teardown_failure(const char *stage) const noexcept
    {
        std::fprintf(stderr, "FAIL private station teardown: %s error=0x%08lx\n", stage,
            static_cast<unsigned long>(GetLastError()));
        std::fflush(stderr);
        std::_Exit(1);
    }
    HWINSTA original_station_ = nullptr;
    HDESK original_desktop_ = nullptr;
    HWINSTA station_ = nullptr;
    HDESK desktop_ = nullptr;
    bool station_changed_ = false;
    bool desktop_changed_ = false;
    std::wstring name_;
    std::wstring desktop_name_;
};

class OleApartment
{
public:
    OleApartment() { require(SUCCEEDED(OleInitialize(nullptr)), "private STA OLE initialization failed"); }
    ~OleApartment() { OleUninitialize(); }
};

class MessageOwner
{
public:
    MessageOwner()
    {
        isolated_clipboard::require_private();
        instance_ = GetModuleHandleW(nullptr);
        name_ = L"HmiPrivateOleOwner_" + std::to_wstring(GetCurrentProcessId());
        WNDCLASSW type{};
        type.hInstance = instance_;
        type.lpszClassName = name_.c_str();
        type.lpfnWndProc = DefWindowProcW;
        atom_ = RegisterClassW(&type);
        require(atom_ != 0, "cannot register private message owner");
        window = CreateWindowExW(0, name_.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, nullptr);
        if (!window)
        {
            UnregisterClassW(name_.c_str(), instance_);
            atom_ = 0;
            throw std::runtime_error("cannot create private message-only owner");
        }
    }
    ~MessageOwner()
    {
        isolated_clipboard::require_private();
        if (window) DestroyWindow(window);
        if (atom_) UnregisterClassW(name_.c_str(), instance_);
    }
    HWND window = nullptr;
private:
    HINSTANCE instance_ = nullptr;
    ATOM atom_ = 0;
    std::wstring name_;
};

struct PrivateClipboardCleanup
{
    ~PrivateClipboardCleanup() { (void)OleSetClipboard(nullptr); }
};

class OwnedWorker
{
public:
    ~OwnedWorker() { drain(); }
    void drain()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (worker.busy())
        {
            isolated_clipboard::require_private();
            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::fputs("FAIL private OLE worker did not finish; refusing to restore interactive station\n", stderr);
                std::fflush(stderr);
                std::_Exit(1);
            }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ClipboardSnapshotWorker worker;
};

struct Medium
{
    STGMEDIUM value{};
    ~Medium() { if (value.tymed != TYMED_NULL) ReleaseStgMedium(&value); }
};

std::vector<unsigned char> text_bytes(const std::wstring &text)
{
    const auto *bytes = reinterpret_cast<const unsigned char *>(text.c_str());
    return {bytes, bytes + (text.size() + 1) * sizeof(wchar_t)};
}

std::vector<unsigned char> html_bytes(int iteration)
{
    const std::string prefix = "<html><body><!--StartFragment-->";
    const std::string fragment = "<b>Original HTML " + std::to_string(iteration) + "</b>";
    const std::string suffix = "<!--EndFragment--></body></html>";
    const std::string prototype = "Version:1.0\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
        "StartFragment:0000000000\r\nEndFragment:0000000000\r\n";
    const auto start = prototype.size();
    char header[256]{};
    const int count = std::snprintf(header, sizeof(header), "Version:1.0\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\n"
        "StartFragment:%010zu\r\nEndFragment:%010zu\r\n", start, start + prefix.size() + fragment.size() + suffix.size(),
        start + prefix.size(), start + prefix.size() + fragment.size());
    require(count > 0 && static_cast<std::size_t>(count) == start, "invalid fixture HTML offsets");
    const std::string html = std::string(header) + prefix + fragment + suffix;
    return {reinterpret_cast<const unsigned char *>(html.c_str()),
        reinterpret_cast<const unsigned char *>(html.c_str()) + html.size() + 1};
}

std::vector<unsigned char> add_global(ClipboardSnapshot &snapshot, UINT format, const std::vector<unsigned char> &bytes)
{
    ClipboardValue value;
    value.format = {static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    value.data = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes.size());
    require(value.data != nullptr, "cannot allocate fixture HGLOBAL");
    value.bytes = GlobalSize(value.data);
    require(value.bytes >= bytes.size(), "fixture HGLOBAL allocation is too small");
    std::vector<unsigned char> stored(value.bytes, 0);
    std::memcpy(stored.data(), bytes.data(), bytes.size());
    auto *destination = static_cast<unsigned char *>(GlobalLock(value.data));
    require(destination != nullptr, "cannot lock fixture HGLOBAL");
    std::memcpy(destination, stored.data(), stored.size());
    GlobalUnlock(value.data);
    snapshot.bytes += value.bytes;
    snapshot.values.push_back(std::move(value));
    return stored;
}

void verify_global(IDataObject *object, UINT format, const std::vector<unsigned char> &expected)
{
    FORMATETC description{static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    Medium medium;
    require(SUCCEEDED(object->GetData(&description, &medium.value)) && medium.value.tymed == TYMED_HGLOBAL,
        "restored HGLOBAL format unavailable");
    const SIZE_T count = GlobalSize(medium.value.hGlobal);
    const auto *bytes = static_cast<const unsigned char *>(GlobalLock(medium.value.hGlobal));
    require(bytes != nullptr, "cannot lock restored HGLOBAL");
    const bool same = count == expected.size() && std::memcmp(bytes, expected.data(), expected.size()) == 0;
    GlobalUnlock(medium.value.hGlobal);
    require(same, "restored HGLOBAL bytes differ");
}

void verify_stream(IDataObject *object, UINT format, const std::vector<unsigned char> &expected)
{
    FORMATETC description{static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM};
    Medium medium;
    require(SUCCEEDED(object->GetData(&description, &medium.value)) && medium.value.tymed == TYMED_ISTREAM &&
        medium.value.pstm, "restored IStream format unavailable");
    ComPtr<IStream> stream;
    require(SUCCEEDED(medium.value.pstm->Clone(&stream)) && stream, "cannot clone restored stream");
    LARGE_INTEGER zero{};
    require(SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)), "cannot rewind restored stream clone");
    STATSTG info{};
    require(SUCCEEDED(stream->Stat(&info, STATFLAG_NONAME)) && info.cbSize.QuadPart == expected.size(),
        "restored stream size differs");
    std::vector<unsigned char> bytes(expected.size());
    ULONG read = 0;
    require(SUCCEEDED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) &&
        read == bytes.size() && bytes == expected, "restored stream bytes differ");
}

void roundtrip(HWND owner, int iteration)
{
    isolated_clipboard::require_private();
    const UINT html_format = RegisterClipboardFormatW(L"HTML Format");
    const UINT stream_format = RegisterClipboardFormatW(L"HmiPrivateOleIntegrationStream");
    require(html_format != 0 && stream_format != 0, "cannot register private fixture formats");
    auto source = std::make_shared<ClipboardSnapshot>();
    const auto unicode = add_global(*source, CF_UNICODETEXT,
        text_bytes(L"Original 原文 \U0001f642 round " + std::to_wstring(iteration)));
    const auto html = add_global(*source, html_format, html_bytes(iteration));
    const std::vector<unsigned char> binary{0, 0xff, 0x80, 's', 't', 'r', 'e', 'a', 'm',
        static_cast<unsigned char>(iteration), 0};
    ClipboardValue stream;
    stream.format = {static_cast<CLIPFORMAT>(stream_format), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM};
    stream.stream = binary;
    stream.bytes = binary.size();
    source->bytes += stream.bytes;
    source->values.push_back(std::move(stream));
    ComPtr<IDataObject> source_object;
    source_object.Attach(new ClipboardDataObject(source));
    require(SUCCEEDED(OleSetClipboard(source_object.Get())), "cannot publish private source IDataObject");
    // The capture worker cannot synchronously call a live main-thread STA while
    // main waits. Materialize all source formats before starting that worker.
    require(SUCCEEDED(OleFlushClipboard()), "cannot materialize private source");
    source_object.Reset();
    OwnedWorker capture;
    std::shared_ptr<const ClipboardSnapshot> snapshot;
    std::string error;
    const bool captured = capture.worker.capture({CF_UNICODETEXT, html_format, stream_format}, snapshot, error);
    require(captured, error.c_str());
    require(snapshot && snapshot->values.size() >= 3, "private snapshot lost a public format");
    require(OpenClipboard(owner) != FALSE, "cannot open private clipboard for temporary text");
    struct CloseClipboardScope { ~CloseClipboardScope() { CloseClipboard(); } };
    DWORD sequence = 0;
    {
        CloseClipboardScope close;
        require(EmptyClipboard() != FALSE, "cannot clear private clipboard");
        const auto temporary = text_bytes(L"Temporary private Unicode text");
        const HGLOBAL text = GlobalAlloc(GMEM_MOVEABLE, temporary.size());
        require(text != nullptr, "cannot allocate temporary private text");
        void *destination = GlobalLock(text);
        if (!destination) { GlobalFree(text); throw std::runtime_error("cannot lock temporary private text"); }
        std::memcpy(destination, temporary.data(), temporary.size());
        GlobalUnlock(text);
        if (!SetClipboardData(CF_UNICODETEXT, text))
        { GlobalFree(text); throw std::runtime_error("cannot publish temporary private text"); }
        sequence = GetClipboardSequenceNumber();
    }
    const unsigned flushes_before_restore = isolated_clipboard::completed_flushes.load();
    const auto restored = capture.worker.restore(snapshot, owner, sequence, error);
    require(restored == ClipboardRestoreResult::Restored || restored == ClipboardRestoreResult::Pending, error.c_str());
    // Real EmptyClipboard inside OleSetClipboard can synchronously notify our
    // main-thread owner. If the bounded call returned pending, pump that owner
    // while its already committed STA finishes the one restoration and Flush.
    capture.drain();
    require(isolated_clipboard::completed_flushes.load() == flushes_before_restore + 1,
        "private restoration did not complete its own OleFlushClipboard");
    ComPtr<IDataObject> result;
    require(SUCCEEDED(OleGetClipboard(&result)) && result, "cannot retrieve restored private IDataObject");
    verify_global(result.Get(), CF_UNICODETEXT, unicode);
    verify_global(result.Get(), html_format, html);
    verify_stream(result.Get(), stream_format, binary);
    std::printf("PASS isolated OS OLE roundtrip=%d formats=%zu bytes=%zu stream_bytes=%zu\n",
        iteration, snapshot->values.size(), snapshot->bytes, binary.size());
}
}

int main()
{
    PrivateStation isolation;
    try
    {
        const int entered = isolation.enter();
        if (entered != 0) return entered;
        OleApartment ole;
        MessageOwner owner;
        PrivateClipboardCleanup cleanup;
        roundtrip(owner.window, 1);
        roundtrip(owner.window, 2);
        std::puts("PASS real OLE integration used only a private noninteractive window station; no keyboard input or application automation");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "FAIL isolated OLE integration: %s\n", error.what());
        return 1;
    }
}
