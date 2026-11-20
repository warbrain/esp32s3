#include "check.hpp"
#include "action/actions.hpp"

#include <stdexcept>

HMI_TEST(action_rejects_unknown_or_missing_parameters_without_side_effects)
{
    using namespace hmi::action;
    const Context context;
    CHECK(!execute({"unknown", {}}, context).ok);
    CHECK(!execute({"launch", {}}, context).ok);
    CHECK(!execute({"key", {{"chord", "Ctrl+A+Delete"}}}, context).ok);
    CHECK(!execute({"key", {{"chord", "Ctrl+"}}}, context).ok);
    CHECK(!execute({"key", {{"chord", "Ctrl+Ctrl+A"}}}, context).ok);
    CHECK(!execute({"key", {{"chord", "NotAKey"}}}, context).ok);
    CHECK(!execute({"wait", {{"ms", "-1"}}}, context).ok);
    CHECK(!execute({"wait", {{"ms", "10001"}}}, context).ok);
    CHECK(!execute({"wait", {{"ms", "1x"}}}, context).ok);
    CHECK(execute({"wait", {{"ms", "0"}}}, context).ok);
    CHECK(!execute({"text", {{"text", "hello"}}}, context).ok);
}

HMI_TEST(action_routes_text_and_speech_through_context_and_catches_failures)
{
    using namespace hmi::action;
    std::string inserted;
    std::string spoken;
    Context context;
    context.insert_text = [&inserted](std::string_view text) { inserted = text; return true; };
    context.speak = [&spoken](std::string_view text) { spoken = text; return true; };
    CHECK(execute({"text", {{"text", "你好"}}}, context).ok);
    CHECK(inserted == "你好");
    CHECK(execute({"speak", {{"text", "hello"}}}, context).via == "tts");
    CHECK(spoken == "hello");
    CHECK(!execute({"text", {{"text", std::string("\xc0\x80", 2)}}}, context).ok);
    CHECK(inserted == "你好");
    context.speak = [](std::string_view) -> bool { throw std::runtime_error("synthesis failed"); };
    const auto result = execute({"speak", {{"text", "hello"}}}, context);
    CHECK(!result.ok);
    CHECK(result.detail == "synthesis failed");
    CHECK(result.elapsed_ms >= 0);
}
