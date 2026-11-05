#include "check.hpp"
#include "action/ui_input.hpp"

HMI_TEST(uia_strategy_prefers_writable_value)
{
    using hmi::action::choose_input_path;
    using hmi::action::InputPath;
    CHECK(choose_input_path(true, true) == InputPath::UiaValue);
    CHECK(choose_input_path(false, true) == InputPath::UiaLegacy);
    CHECK(choose_input_path(false, false) == InputPath::SendInput);
}

HMI_TEST(forced_text_routing_is_respected)
{
    using hmi::action::choose_input_path;
    using hmi::action::InputPath;
    CHECK(choose_input_path(true, true, "sendinput") == InputPath::SendInput);
    CHECK(choose_input_path(false, false, "uia") == InputPath::None);
    CHECK(choose_input_path(true, true, "uia") == InputPath::UiaValue);
    CHECK(choose_input_path(false, true, "uia") == InputPath::UiaLegacy);
    CHECK(choose_input_path(true, true, "invalid") == InputPath::None);
}

HMI_TEST(invalid_utf8_is_rejected_before_touching_foreground)
{
    hmi::action::TextInput input;
    const std::string invalid("\xc0\x80", 2);
    CHECK(!input.insert(invalid).ok);
    CHECK(!input.insert("").ok);
    CHECK(!input.insert(std::string("a\0b", 3)).ok);
    CHECK(!input.insert(std::string(16385, 'a')).ok);
    CHECK(!hmi::action::insert_text(invalid).ok);
    CHECK(!hmi::action::insert_text("").ok);
}

HMI_TEST(invalid_text_routing_is_rejected_before_input)
{
    hmi::action::TextInput input;
    for (const std::string_view route : {"UIA", "", "uia ", "sendinputx", "automatic"})
    {
        const auto result = input.insert("must not be sent", route);
        CHECK(!result.ok);
        CHECK(result.path == hmi::action::InputPath::None);
    }
}
