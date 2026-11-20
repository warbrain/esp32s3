#include "intent/rule.hpp"

#include <cmath>
#include <regex>

namespace hmi::intent
{
std::string normalize(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size();)
    {
        const auto first = static_cast<unsigned char>(text[index]);
        if (index + 2 < text.size() && (first == 0xef || first == 0xe3))
        {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80)
            {
                const unsigned code = ((first & 15u) << 12) | ((second & 63u) << 6) | (third & 63u);
                if (code == 0x3000 || (code >= 0xff01 && code <= 0xff5e))
                {
                    char c = code == 0x3000 ? ' ' : static_cast<char>(code - 0xfee0);
                    if (c >= 'A' && c <= 'Z')
                        c = static_cast<char>(c + ('a' - 'A'));
                    result.push_back(c);
                    index += 3;
                    continue;
                }
            }
        }
        char c = text[index++];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
        result.push_back(c);
    }
    const auto first = result.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string::npos)
        return {};
    const auto last = result.find_last_not_of(" \t\r\n\f\v");
    return result.substr(first, last - first + 1);
}

Rule *match(RuleSet &rules, std::string_view text, float confidence, std::int64_t now_ms,
            std::string *matched_phrase)
{
    if (matched_phrase)
        matched_phrase->clear();
    if (now_ms < 0 || !std::isfinite(confidence) || confidence > 1.0f || text.size() > 8192)
        return nullptr;
    const std::string normalized = normalize(text);
    if (normalized.empty())
        return nullptr;
    for (Rule &rule : rules.rules)
    {
        if (!rule.enabled || rule.trigger.kind == TriggerKind::Hotkey ||
            !std::isfinite(rule.trigger.min_confidence) || rule.trigger.min_confidence < 0.0f ||
            rule.trigger.min_confidence > 1.0f ||
            (confidence < 0.0f ? rule.trigger.min_confidence > 0.0f : confidence < rule.trigger.min_confidence))
            continue;
        // Zero is the frozen contract's "never fired" sentinel. Encode a hit
        // at the clock origin as -1 so that a second utterance respects cooldown.
        if (rule.last_fired_ms != 0)
        {
            const std::int64_t last = rule.last_fired_ms < 0 ? 0 : rule.last_fired_ms;
            if (now_ms < last || now_ms - last < rule.cooldown_ms)
                continue;
        }
        const std::string pattern = normalize(rule.trigger.pattern);
        if (pattern.empty() || pattern.size() > 1024)
            continue;
        bool found = false;
        std::string phrase;
        if (rule.trigger.kind == TriggerKind::Phrase)
        {
            found = normalized.find(pattern) != std::string::npos;
            phrase = pattern;
        }
        else if (rule.trigger.kind == TriggerKind::Regex)
        {
            try
            {
                std::smatch result;
                found = std::regex_search(normalized, result,
                    std::regex(rule.trigger.pattern, std::regex::ECMAScript | std::regex::icase));
                if (found)
                    phrase = result.str();
            }
            catch (const std::regex_error &)
            {
                continue;
            }
        }
        if (!found)
            continue;
        rule.last_fired_ms = now_ms == 0 ? -1 : now_ms;
        if (matched_phrase)
            *matched_phrase = std::move(phrase);
        return &rule;
    }
    return nullptr;
}
}
