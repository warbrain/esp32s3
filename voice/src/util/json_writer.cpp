#include "util/json_writer.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hmi::util::json
{
namespace
{
void quote(std::string &out, std::string_view text)
{
    constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        }
        else if (c < 0x20)
        {
            out += "\\u00";
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
        else
            out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
}

void write(std::string &out, const Value &value, unsigned depth)
{
    if (depth > 64)
        throw std::invalid_argument("JSON nesting exceeds 64 levels");
    if (std::holds_alternative<std::nullptr_t>(value.data))
        out += "null";
    else if (const auto *boolean = std::get_if<bool>(&value.data))
        out += *boolean ? "true" : "false";
    else if (const auto *number = std::get_if<double>(&value.data))
    {
        if (!std::isfinite(*number))
            throw std::invalid_argument("cannot serialize non-finite JSON number");
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), *number,
                                          std::chars_format::general, std::numeric_limits<double>::max_digits10);
        if (result.ec != std::errc{})
            throw std::invalid_argument("cannot serialize JSON number");
        out.append(buffer, result.ptr);
    }
    else if (const auto *text = std::get_if<std::string>(&value.data))
        quote(out, *text);
    else if (const auto *array = std::get_if<Value::Array>(&value.data))
    {
        out.push_back('[');
        bool separator = false;
        for (const auto &element : *array)
        {
            if (separator)
                out.push_back(',');
            separator = true;
            write(out, element, depth + 1);
        }
        out.push_back(']');
    }
    else
    {
        out.push_back('{');
        bool separator = false;
        for (const auto &[key, element] : std::get<Value::Object>(value.data))
        {
            if (separator)
                out.push_back(',');
            separator = true;
            quote(out, key);
            out.push_back(':');
            write(out, element, depth + 1);
        }
        out.push_back('}');
    }
}
}

std::string stringify(const Value &value)
{
    std::string out;
    write(out, value, 0);
    return out;
}
}
