#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hmi::intent
{
enum class TriggerKind
{
    Phrase, // substring match after normalize()
    Regex,  // std::regex, ECMAScript
    Hotkey  // reserved for V2
};

struct Trigger
{
    TriggerKind kind = TriggerKind::Phrase;
    std::string pattern;
    float min_confidence = 0.55f;
};

struct Action
{
    std::string kind; // launch | key | text | speak | wait
    std::map<std::string, std::string, std::less<>> params;
};

struct Rule
{
    std::string id;
    std::string name;
    bool enabled = true;
    Trigger trigger;
    std::vector<Action> actions;
    int cooldown_ms = 800;

    // Stamped by match() so the next utterance respects the cooldown.
    std::int64_t last_fired_ms = 0;
};

struct RuleSet
{
    std::vector<Rule> rules;
    std::string source_path;
};

// Full-width -> half-width, trim, lowercase. Used by every trigger kind.
std::string normalize(std::string_view text);

// First enabled rule that matches and whose cooldown elapsed. Stamps
// last_fired_ms on the returned rule and, when non-null, writes the matched
// phrase into matched_phrase.
Rule *match(RuleSet &rules, std::string_view text, float confidence, std::int64_t now_ms,
            std::string *matched_phrase = nullptr);
}
