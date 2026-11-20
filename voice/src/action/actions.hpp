#pragma once

#include "intent/rule.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace hmi::action
{
struct Result
{
    bool ok = false;
    std::string detail;
    double elapsed_ms = 0.0;
    std::string via; // "launch" | "key" | "wait" | "uia:value" | "sendinput" | "tts"
};

// Callbacks for actions that need another layer, so this file never depends on
// the engine: text -> UIA insertion, speak -> TTS synthesis.
struct Context
{
    std::function<bool(std::string_view text)> insert_text;
    std::function<bool(std::string_view text)> speak;
};

// Whitelisted executors. Unknown kinds return ok=false, never throw.
Result execute(const intent::Action &action, const Context &context);
}
