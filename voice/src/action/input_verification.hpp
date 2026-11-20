#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hmi::action::detail
{
inline constexpr std::size_t maximum_verification_text_units = 65536;
inline constexpr std::size_t maximum_verification_insert_units = 16384;

enum class VerificationOutcome
{
    Unavailable,
    Verified,
    Mismatch
};

struct InputTextSnapshot
{
    // The observer must obtain a coherent, read-only observation of one control.
    // A bounded GetText(limit + 1) result is incomplete if it exceeds limit.
    // Providers whose text and selection cannot be mapped reliably stay unavailable.
    bool available = false;
    bool complete = false;
    bool selection_available = false;
    std::wstring text;
    // Half-open offsets in this exact text, measured in UTF-16 code units.
    // UIA TextUnit_Character counts are not necessarily UTF-16 offsets.
    std::size_t selection_begin = 0;
    std::size_t selection_end = 0;
};

struct InputVerificationResult
{
    VerificationOutcome outcome = VerificationOutcome::Unavailable;
    std::string_view reason = "snapshot_unavailable";
};

inline std::string_view verification_outcome_name(VerificationOutcome outcome) noexcept
{
    switch (outcome)
    {
    case VerificationOutcome::Verified: return "verified";
    case VerificationOutcome::Mismatch: return "mismatch";
    case VerificationOutcome::Unavailable: return "unavailable";
    }
    return "unavailable";
}

namespace input_verification
{
inline bool high_surrogate(std::uint32_t value) noexcept { return value >= 0xd800 && value <= 0xdbff; }
inline bool low_surrogate(std::uint32_t value) noexcept { return value >= 0xdc00 && value <= 0xdfff; }

inline bool valid_utf16(std::wstring_view text) noexcept
{
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        const auto unit = static_cast<std::uint32_t>(text[index]);
        if (unit == 0 || unit > 0xffff || low_surrogate(unit)) return false;
        if (high_surrogate(unit))
        {
            if (++index == text.size() || !low_surrogate(static_cast<std::uint32_t>(text[index]))) return false;
        }
    }
    return true;
}

inline bool valid_boundary(std::wstring_view text, std::size_t offset) noexcept
{
    return offset <= text.size() && (offset == 0 || offset == text.size() ||
        !(high_surrogate(static_cast<std::uint32_t>(text[offset - 1])) &&
          low_surrogate(static_cast<std::uint32_t>(text[offset]))));
}

inline bool valid_selection(const InputTextSnapshot &snapshot) noexcept
{
    return snapshot.selection_begin <= snapshot.selection_end &&
        valid_boundary(snapshot.text, snapshot.selection_begin) && valid_boundary(snapshot.text, snapshot.selection_end);
}
}

// Pure verification only: no COM, writes, retries, clipboard access or threads.
// The caller proves control identity and limits polling in its existing worker.
// Use the final settled observation of that bounded after-poll: an intermediate
// mismatch is not permission to resend input that may still be in the event queue.
// A verified change is an exact UTF-16 insertion/replacement at the captured
// selection, with unchanged surrounding text and a consistent resulting caret.
// It does not establish causality if somebody makes the same edit concurrently.
inline InputVerificationResult verify_input_change(const InputTextSnapshot &before,
                                                   const InputTextSnapshot &after,
                                                   std::wstring_view inserted,
                                                   bool same_control) noexcept
{
    using namespace input_verification;
    if (inserted.empty()) return {VerificationOutcome::Unavailable, "empty_insertion"};
    if (inserted.size() > maximum_verification_insert_units)
        return {VerificationOutcome::Unavailable, "insertion_too_large"};
    if (!valid_utf16(inserted)) return {VerificationOutcome::Unavailable, "invalid_insertion_utf16"};
    if (!same_control) return {VerificationOutcome::Unavailable, "control_changed"};
    if (!before.available || !after.available) return {};
    if (!before.complete || !after.complete) return {VerificationOutcome::Unavailable, "snapshot_incomplete"};
    if (before.text.size() > maximum_verification_text_units || after.text.size() > maximum_verification_text_units)
        return {VerificationOutcome::Unavailable, "snapshot_too_large"};
    if (!valid_utf16(before.text) || !valid_utf16(after.text))
        return {VerificationOutcome::Unavailable, "invalid_snapshot_utf16"};
    if (!before.selection_available || !after.selection_available)
        return {VerificationOutcome::Unavailable, "selection_unavailable"};
    if (!valid_selection(before) || !valid_selection(after))
        return {VerificationOutcome::Unavailable, "invalid_selection"};
    if (before.text == after.text) return {VerificationOutcome::Unavailable, "no_observable_change"};

    const std::wstring_view original = before.text;
    const std::wstring_view observed = after.text;
    const auto prefix = original.substr(0, before.selection_begin);
    const auto suffix = original.substr(before.selection_end);
    const auto retained_units = prefix.size() + suffix.size();
    if (inserted.size() > maximum_verification_text_units - retained_units)
        return {VerificationOutcome::Unavailable, "expected_text_too_large"};
    if (observed.size() < retained_units || observed.substr(0, prefix.size()) != prefix ||
        observed.substr(observed.size() - suffix.size()) != suffix)
        return {VerificationOutcome::Unavailable, "surrounding_text_changed"};

    const auto actual_units = observed.size() - retained_units;
    const auto caret = prefix.size() + actual_units;
    if (after.selection_begin != caret || after.selection_end != caret)
        return {VerificationOutcome::Unavailable, "selection_changed"};
    const auto actual = observed.substr(prefix.size(), actual_units);
    return actual == inserted ? InputVerificationResult{VerificationOutcome::Verified, "exact_insertion"} :
        InputVerificationResult{VerificationOutcome::Mismatch, "inserted_text_mismatch"};
}
}
