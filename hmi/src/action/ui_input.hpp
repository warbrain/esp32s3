#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hmi::action
{
enum class InputPath
{
    None,
    UiaValue,  // ValuePattern::SetValue
    UiaText,   // Reserved: TextPattern2 exposes no insertion API.
    UiaLegacy, // LegacyIAccessiblePattern::SetValue
    SendInput  // KEYEVENTF_UNICODE fallback
};

enum class InputDelivery
{
    NotSent,
    DeliveredUnverified,
    Verified,
    Mismatch,
    Unknown
};

inline std::string_view input_delivery_name(InputDelivery delivery) noexcept
{
    switch (delivery)
    {
    case InputDelivery::DeliveredUnverified: return "delivered_unverified";
    case InputDelivery::Verified: return "verified";
    case InputDelivery::Mismatch: return "mismatch";
    case InputDelivery::Unknown: return "unknown";
    default: return "not_sent";
    }
}

struct Target
{
    unsigned long pid = 0;
    std::string process; // ex: "notepad.exe"
    std::string window;  // foreground window title, UTF-8
    std::uintptr_t native_window = 0; // Captured together with pid; never rebind during input.
    std::uintptr_t native_focus = 0; // Optional release-time focused child, never rebound.
};

struct InsertResult
{
    bool ok = false;
    InputPath path = InputPath::None;
    std::string detail;
    double total_ms = -1.0;
    double uia_ms = -1.0; // Caller-observed lookup/write wait, including cancellation.
    double uia_worker_ms = -1.0; // Available only after the worker result is ready.
    double send_input_ms = -1.0; // Cumulative SendInput API time, including release-only cleanup.
    double clipboard_ms = -1.0; // Complete clipboard transaction, including restoration.
    bool uia_attempted = false;
    bool send_input_attempted = false;
    bool clipboard_attempted = false;
    bool uia_timed_out = false;
    std::uint32_t send_input_calls = 0;
    InputDelivery delivery = InputDelivery::NotSent;
    bool verification_attempted = false;
    double verification_before_ms = -1.0;
    double verification_after_ms = -1.0;
    std::string verification_reason;
    std::string control_class;
    int control_unicode = -1; // Negative when no native edit control is available.
    std::uint32_t modifiers = 0; // Ctrl/Alt/Shift/Windows at the delivery boundary.
    bool modifiers_observed = false;
    std::string font_name; // UIA report only, not proof that glyphs render correctly.
};

// Foreground window identity, captured before an action runs so the debug
// console can tell which application received the input.
Target foreground_target();

// Owns at most one outstanding UIA operation. Cross-process COM may ignore
// cancellation; a timed-out operation stays owned until it returns, and no
// second UIA thread is started meanwhile. Call insert() from a worker thread.
class TextInput
{
public:
    TextInput();
    ~TextInput();
    TextInput(const TextInput &) = delete;
    TextInput &operator=(const TextInput &) = delete;

    // routing is exactly "auto", "uia", or "sendinput". Forced UIA never
    // falls back. Auto never retries a write with an unknown outcome.
    InsertResult insert(std::string_view utf8, std::string_view routing = "auto");
    // Production actions pass the release-time target. A changed HWND or PID
    // cancels input, including in the UIA worker and SendInput fallback.
    InsertResult insert(std::string_view utf8, const Target &target,
                        std::string_view routing = "auto");

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Compatibility path without a persistent owner: validated Unicode SendInput.
// Production callers keep TextInput alive to use the bounded UIA fallback.
InsertResult insert_text(std::string_view utf8);

// Pure strategy used after inspecting the focused element.
InputPath choose_input_path(bool value_writable, bool legacy_writable) noexcept;
InputPath choose_input_path(bool value_writable, bool legacy_writable,
                            std::string_view routing) noexcept;
}
