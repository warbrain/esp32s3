#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hmi::engine::detail
{
namespace tts_segmentation
{
struct Codepoint
{
    std::size_t begin = 0;
    std::size_t end = 0;
    std::uint32_t value = 0;
};

inline bool decode(std::string_view text, std::vector<Codepoint> &out)
{
    for (std::size_t cursor = 0; cursor < text.size();)
    {
        const auto begin = cursor;
        const auto lead = static_cast<unsigned char>(text[cursor++]);
        std::uint32_t value = lead;
        std::size_t continuation = 0;
        if (lead < 0x80U) {}
        else if (lead >= 0xc2U && lead <= 0xdfU) { value &= 0x1fU; continuation = 1; }
        else if (lead >= 0xe0U && lead <= 0xefU) { value &= 0x0fU; continuation = 2; }
        else if (lead >= 0xf0U && lead <= 0xf4U) { value &= 0x07U; continuation = 3; }
        else return false;
        if (continuation > text.size() - cursor) return false;
        for (std::size_t index = 0; index < continuation; ++index)
        {
            const auto next = static_cast<unsigned char>(text[cursor++]);
            if ((next & 0xc0U) != 0x80U) return false;
            value = (value << 6) | (next & 0x3fU);
        }
        if ((continuation == 1 && value < 0x80U) || (continuation == 2 && value < 0x800U) ||
            (continuation == 3 && value < 0x10000U) || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU) || value == 0) return false;
        out.push_back({begin, cursor, value});
    }
    return true;
}

inline bool whitespace(std::uint32_t value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
        value == 0xa0U || value == 0x3000U;
}

inline bool punctuation(std::uint32_t value)
{
    return (value >= 0x21U && value <= 0x2fU) || (value >= 0x3aU && value <= 0x40U) ||
        (value >= 0x5bU && value <= 0x60U) || (value >= 0x7bU && value <= 0x7eU) ||
        (value >= 0x2000U && value <= 0x206fU) || (value >= 0x3001U && value <= 0x303fU) ||
        value == 0xff01U || value == 0xff0cU || value == 0xff0eU || value == 0xff1aU ||
        value == 0xff1bU || value == 0xff1fU;
}

inline bool spoken(std::uint32_t value) { return !whitespace(value) && !punctuation(value); }
inline bool digit(std::uint32_t value)
{
    return (value >= '0' && value <= '9') || (value >= 0xff10U && value <= 0xff19U);
}

struct NumericToken
{
    std::size_t begin = 0;
    std::size_t end = 0;
};

inline bool numeric_separator(std::uint32_t value)
{
    return value == '.' || value == ',' || value == ':' || value == 0xff0eU ||
        value == 0xff0cU || value == 0xff1aU;
}

inline std::vector<NumericToken> numeric_tokens(const std::vector<Codepoint> &points)
{
    std::vector<NumericToken> tokens;
    for (std::size_t index = 0; index < points.size();)
    {
        if (!digit(points[index].value)) { ++index; continue; }
        const auto begin = index++;
        while (index < points.size())
        {
            if (digit(points[index].value)) ++index;
            else if (numeric_separator(points[index].value) && index + 1 < points.size() &&
                     digit(points[index + 1].value)) index += 2;
            else break;
        }
        tokens.push_back({begin, index});
    }
    return tokens;
}

inline bool ascii_letter(std::uint32_t value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

inline std::vector<NumericToken> protected_tokens(const std::vector<Codepoint> &points)
{
    auto tokens = numeric_tokens(points);
    for (std::size_t index = 0; index < points.size();)
    {
        if (!ascii_letter(points[index].value)) { ++index; continue; }
        const auto begin = index++;
        while (index < points.size())
        {
            if (ascii_letter(points[index].value)) ++index;
            else if ((points[index].value == '\'' || points[index].value == '-' ||
                      points[index].value == 0x2019U) && index + 1 < points.size() &&
                     ascii_letter(points[index + 1].value)) index += 2;
            else break;
        }
        tokens.push_back({begin, index});
    }
    std::sort(tokens.begin(), tokens.end(), [](const NumericToken &left, const NumericToken &right) {
        return left.begin < right.begin;
    });
    return tokens;
}

inline const NumericToken *token_at_boundary(const std::vector<NumericToken> &tokens, std::size_t boundary)
{
    const auto token = std::lower_bound(tokens.begin(), tokens.end(), boundary,
        [](const NumericToken &item, std::size_t end) { return item.end <= end; });
    return token != tokens.end() && token->begin < boundary && boundary < token->end &&
        token->end - token->begin <= 64 ? &*token : nullptr;
}

inline bool strong_boundary(const std::vector<Codepoint> &points, std::size_t index)
{
    const auto value = points[index].value;
    if (value == '.' && index > 0 && index + 1 < points.size() &&
        digit(points[index - 1].value) && digit(points[index + 1].value)) return false;
    return value == '.' || value == '!' || value == '?' || value == ';' || value == '\n' ||
        value == 0x3002U || value == 0xff01U || value == 0xff1fU || value == 0xff1bU;
}

inline bool soft_boundary(std::uint32_t value)
{
    return whitespace(value) || value == ',' || value == ':' || value == 0xff0cU ||
        value == 0xff1aU || value == 0x3001U;
}
}

// Preserve every input byte while bounding normal speech segments in codepoints.
// Leading/trailing punctuation stays with speech rather than becoming a request.
inline std::vector<std::string> split_tts_text(std::string_view text)
{
    using namespace tts_segmentation;
    std::vector<Codepoint> points;
    if (!decode(text, points)) return {};
    const auto tokens = protected_tokens(points);
    std::vector<std::string> segments;
    std::size_t previous_spoken = 0;
    for (std::size_t start = 0; start < points.size();)
    {
        auto content = start;
        while (content < points.size() && !spoken(points[content].value)) ++content;
        if (content == points.size())
        {
            if (!segments.empty()) segments.back().append(text.substr(points[start].begin));
            break;
        }
        const bool first = segments.empty();
        // Ramp up after a short opening so its brief playback can cover the
        // next synthesis instead of jumping directly to a full-size segment.
        const std::size_t maximum = first ? 12 :
            std::clamp(previous_spoken * 2, std::size_t{8}, std::size_t{64});
        const std::size_t minimum_soft = segments.empty() ? 8 : 24;
        const auto limit = std::min(points.size(), std::max(start + maximum, content + 1));
        auto end = limit;
        std::size_t soft = 0;
        for (auto index = content; index < limit; ++index)
        {
            if (strong_boundary(points, index)) { end = index + 1; break; }
            const bool inside_number = index > content && index + 1 < points.size() &&
                digit(points[index - 1].value) && digit(points[index + 1].value);
            // Keep subsequent clauses intact too: a hard cut after a comma
            // can strand a word fragment at the start of the next SDK request.
            // Spaces keep their existing length-boundary fallback so a short
            // English word alone does not create an eager first utterance.
            if (index + 1 - content >= (first ? 3U : 6U) && soft_boundary(points[index].value) &&
                !whitespace(points[index].value) && !inside_number)
            {
                std::size_t remaining = 0;
                for (auto tail = index + 1; tail < points.size() && remaining < 4; ++tail)
                    if (spoken(points[tail].value)) ++remaining;
                // Keep a tiny remainder with its opening instead of creating
                // a second request for fewer than four spoken codepoints.
                end = remaining >= 4 ? index + 1 : points.size();
                break;
            }
            if (index + 1 - start >= minimum_soft && soft_boundary(points[index].value)) soft = index + 1;
            if (index + 1 == limit && soft != 0 && limit < points.size()) end = soft;
        }
        // A growing playback budget is a target, not a reason to cut a nearby
        // Chinese clause mid-word. Keep a small bounded lookahead for its next
        // punctuation mark; unpunctuated text keeps the existing hard budget.
        if (end == limit && end < points.size() &&
            !strong_boundary(points, end - 1) && !soft_boundary(points[end - 1].value) &&
            token_at_boundary(tokens, end) == nullptr)
        {
            const auto extended_limit = std::min({points.size(), limit + 8, start + 64});
            for (auto index = limit; index < extended_limit; ++index)
            {
                const bool inside_number = index > content && index + 1 < points.size() &&
                    digit(points[index - 1].value) && digit(points[index + 1].value);
                if (strong_boundary(points, index) || (!inside_number &&
                    soft_boundary(points[index].value) && !whitespace(points[index].value)))
                {
                    end = index + 1;
                    break;
                }
            }
        }
        // Keep normal English words and numeric values whole. Exceptionally
        // long identifiers retain the normal bounded segment budget.
        if (const auto *token = token_at_boundary(tokens, end))
        {
            end = token->begin > content ? token->begin : token->end;
        }
        // Attach separators and closing punctuation to the preceding spoken part.
        while (end < points.size() && !spoken(points[end].value)) ++end;
        const auto begin_byte = points[start].begin;
        const auto end_byte = points[end - 1].end;
        segments.emplace_back(text.substr(begin_byte, end_byte - begin_byte));
        previous_spoken = 0;
        for (auto index = content; index < end; ++index)
            if (spoken(points[index].value)) ++previous_spoken;
        start = end;
    }
    return segments;
}

// Split only a failed segment, preserving its bytes and protected tokens. An
// indivisible word/value returns no replacement rather than repeating itself.
inline std::vector<std::string> split_tts_recovery(std::string_view text)
{
    using namespace tts_segmentation;
    std::vector<Codepoint> points;
    if (!decode(text, points)) return {};
    const auto tokens = protected_tokens(points);
    std::size_t total_spoken = 0;
    for (const auto &point : points) if (spoken(point.value)) ++total_spoken;
    if (total_spoken < 2) return {};
    std::size_t left_spoken = 0;
    std::size_t best = 0;
    std::size_t distance = total_spoken;
    for (std::size_t boundary = 1; boundary < points.size(); ++boundary)
    {
        if (spoken(points[boundary - 1].value)) ++left_spoken;
        if (left_spoken == 0 || left_spoken == total_spoken || token_at_boundary(tokens, boundary)) continue;
        // Attach unspoken separators to the first half, just as normal splitting does.
        if (!spoken(points[boundary].value)) continue;
        const auto doubled = left_spoken * 2;
        const auto candidate = doubled > total_spoken ? doubled - total_spoken : total_spoken - doubled;
        if (candidate < distance) { best = boundary; distance = candidate; }
    }
    if (best == 0) return {};
    const auto byte = points[best].begin;
    return {std::string(text.substr(0, byte)), std::string(text.substr(byte))};
}
}
