#include "check.hpp"
#include "action/input_verification.hpp"

#include <utility>

namespace
{
using hmi::action::detail::InputTextSnapshot;
using hmi::action::detail::VerificationOutcome;
using hmi::action::detail::verification_outcome_name;
using hmi::action::detail::verify_input_change;

InputTextSnapshot snapshot(std::wstring text, std::size_t begin, std::size_t end)
{
    return {true, true, true, std::move(text), begin, end};
}
}

HMI_TEST(input_verification_checks_caret_insertion_and_preserves_the_existing_draft)
{
    const auto before = snapshot(L"草稿：后文", 3, 3);
    const auto after = snapshot(L"草稿：中文后文", 5, 5);
    CHECK(verify_input_change(before, after, L"中文", true).outcome == VerificationOutcome::Verified);
    CHECK(verify_input_change(before, snapshot(L"中文", 2, 2), L"中文", true).outcome == VerificationOutcome::Unavailable);
    const auto empty = snapshot(L"", 0, 0);
    CHECK(verify_input_change(empty, snapshot(L"中文", 2, 2), L"中文", true).outcome == VerificationOutcome::Verified);
}

HMI_TEST(input_verification_checks_selection_replacement_and_observes_wrong_character_counts)
{
    const auto before = snapshot(L"前文旧字后文", 2, 4);
    CHECK(verify_input_change(before, snapshot(L"前文新文字后文", 5, 5), L"新文字", true).outcome == VerificationOutcome::Verified);
    const auto wrong = verify_input_change(before, snapshot(L"前文????后文", 6, 6), L"新文字", true);
    CHECK(wrong.outcome == VerificationOutcome::Mismatch && wrong.reason == "inserted_text_mismatch");
    CHECK(verify_input_change(before, snapshot(L"前文后文", 2, 2), L"新文字", true).outcome == VerificationOutcome::Mismatch);
}

HMI_TEST(input_verification_never_uses_an_existing_substring_as_write_confirmation)
{
    const auto before = snapshot(L"中文：旧草稿", 6, 6);
    const auto unchanged = verify_input_change(before, before, L"中文", true);
    CHECK(unchanged.outcome == VerificationOutcome::Unavailable && unchanged.reason == "no_observable_change");
    CHECK(verify_input_change(before, snapshot(L"中文：旧草稿中文中文", 10, 10), L"中文", true).outcome == VerificationOutcome::Mismatch);
    // The same selected text cannot demonstrate a write, even if selection collapsed.
    const auto selected = snapshot(L"前中文后", 1, 3);
    CHECK(verify_input_change(selected, snapshot(L"前中文后", 3, 3), L"中文", true).outcome == VerificationOutcome::Unavailable);
}

HMI_TEST(input_verification_requires_reliable_identity_complete_text_and_selection)
{
    const auto before = snapshot(L"前后", 1, 1);
    const auto after = snapshot(L"前新后", 2, 2);
    CHECK(verify_input_change(before, after, L"新", false).outcome == VerificationOutcome::Unavailable);
    auto missing = before;
    missing.available = false;
    CHECK(verify_input_change(missing, after, L"新", true).outcome == VerificationOutcome::Unavailable);
    for (bool observing_before : {false, true})
    {
        auto first = before;
        auto last = after;
        (observing_before ? first : last).complete = false;
        CHECK(verify_input_change(first, last, L"新", true).outcome == VerificationOutcome::Unavailable);
        first = before;
        last = after;
        (observing_before ? first : last).selection_available = false;
        CHECK(verify_input_change(first, last, L"新", true).outcome == VerificationOutcome::Unavailable);
    }
    CHECK(verify_input_change(before, snapshot(L"别的新后", 3, 3), L"新", true).outcome == VerificationOutcome::Unavailable);
    CHECK(verify_input_change(before, snapshot(L"前新后", 0, 0), L"新", true).outcome == VerificationOutcome::Unavailable);
    CHECK(verify_input_change(before, snapshot(L"前新后", 1, 2), L"新", true).outcome == VerificationOutcome::Unavailable);
}

HMI_TEST(input_verification_uses_utf16_offsets_without_splitting_surrogate_pairs)
{
    const std::wstring smile = L"\xd83d\xde42";
    const auto before = snapshot(L"A" + smile + L"B", 3, 3);
    CHECK(verify_input_change(before, snapshot(L"A" + smile + L"新B", 4, 4), L"新", true).outcome == VerificationOutcome::Verified);
    CHECK(verify_input_change(snapshot(L"AB", 1, 1), snapshot(L"A" + smile + L"B", 3, 3), smile, true).outcome == VerificationOutcome::Verified);
    auto split_pair = before;
    split_pair.selection_begin = split_pair.selection_end = 2;
    CHECK(verify_input_change(split_pair, snapshot(L"A新B", 2, 2), L"新", true).outcome == VerificationOutcome::Unavailable);
    CHECK(verify_input_change(before, snapshot(L"A\xd83d新B", 3, 3), L"新", true).outcome == VerificationOutcome::Unavailable);
}

HMI_TEST(input_verification_rejects_empty_invalid_and_unbounded_observations)
{
    using namespace hmi::action::detail;
    const auto before = snapshot(L"", 0, 0);
    const auto after = snapshot(L"新", 1, 1);
    CHECK(verify_input_change(before, after, L"", true).outcome == VerificationOutcome::Unavailable);
    CHECK(verify_input_change(before, after, std::wstring(1, L'\xd83d'), true).outcome == VerificationOutcome::Unavailable);
    CHECK(verify_input_change(before, after, std::wstring(L"a\0b", 3), true).outcome == VerificationOutcome::Unavailable);
    CHECK(verify_input_change(before, after, std::wstring(maximum_verification_insert_units + 1, L'x'), true).outcome == VerificationOutcome::Unavailable);
    const auto oversized = snapshot(std::wstring(maximum_verification_text_units + 1, L'x'), 0, 0);
    CHECK(verify_input_change(oversized, after, L"新", true).outcome == VerificationOutcome::Unavailable);
    CHECK(verify_input_change(before, oversized, L"新", true).outcome == VerificationOutcome::Unavailable);
    const auto at_limit = snapshot(std::wstring(maximum_verification_text_units, L'x'), 0, 0);
    CHECK(verify_input_change(at_limit, snapshot(std::wstring(maximum_verification_text_units - 1, L'x'), 0, 0), L"新", true).reason == "expected_text_too_large");
    auto replacing_at_limit = at_limit;
    replacing_at_limit.selection_end = 1;
    CHECK(verify_input_change(replacing_at_limit,
        snapshot(L"新" + std::wstring(maximum_verification_text_units - 1, L'x'), 1, 1), L"新", true).outcome == VerificationOutcome::Verified);
}

HMI_TEST(input_verification_rejects_selection_metadata_that_cannot_describe_the_snapshot)
{
    const auto after = snapshot(L"前新后", 2, 2);
    CHECK(verify_input_change(snapshot(L"前后", 2, 1), after, L"新", true).outcome == VerificationOutcome::Unavailable);
    CHECK(verify_input_change(snapshot(L"前后", 3, 3), after, L"新", true).outcome == VerificationOutcome::Unavailable);
    CHECK(verify_input_change(snapshot(L"前后", 1, 1), snapshot(L"前新后", 4, 4), L"新", true).outcome == VerificationOutcome::Unavailable);
    CHECK(verification_outcome_name(VerificationOutcome::Verified) == "verified");
    CHECK(verification_outcome_name(VerificationOutcome::Mismatch) == "mismatch");
    CHECK(verification_outcome_name(VerificationOutcome::Unavailable) == "unavailable");
}
