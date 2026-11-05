#include "util/json_reader.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace hmi::util::json
{
namespace
{
void append_utf8(std::string &out, std::uint32_t value)
{
    if (value <= 0x7f)
        out.push_back(static_cast<char>(value));
    else if (value <= 0x7ff)
    {
        out.push_back(static_cast<char>(0xc0 | (value >> 6)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
    else if (value <= 0xffff)
    {
        out.push_back(static_cast<char>(0xe0 | (value >> 12)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
    else
    {
        out.push_back(static_cast<char>(0xf0 | (value >> 18)));
        out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
}

class Reader
{
public:
    explicit Reader(std::string_view text) : text_(text) {}

    Value read(unsigned depth = 0)
    {
        if (depth > 64)
            fail("nesting exceeds 64 levels");
        whitespace();
        if (position_ == text_.size())
            fail("expected value");
        const char next = text_[position_];
        if (next == '"')
            return Value(string());
        if (next == '{')
        {
            ++position_;
            Value::Object object;
            if (take('}'))
                return Value(std::move(object));
            do
            {
                whitespace();
                const std::string key = string();
                require(':');
                if (!object.emplace(key, read(depth + 1)).second)
                    fail("duplicate object key");
                if (take('}'))
                    return Value(std::move(object));
                require(',');
            } while (true);
        }
        if (next == '[')
        {
            ++position_;
            Value::Array array;
            if (take(']'))
                return Value(std::move(array));
            do
            {
                array.push_back(read(depth + 1));
                if (take(']'))
                    return Value(std::move(array));
                require(',');
            } while (true);
        }
        if (next == 't')
        {
            literal("true");
            return Value(true);
        }
        if (next == 'f')
        {
            literal("false");
            return Value(false);
        }
        if (next == 'n')
        {
            literal("null");
            return {};
        }
        return number();
    }

    bool finished()
    {
        whitespace();
        return position_ == text_.size();
    }

private:
    std::string_view text_;
    std::size_t position_ = 0;

    [[noreturn]] void fail(const char *message) const
    {
        throw std::runtime_error(std::string(message) + " at byte " + std::to_string(position_));
    }

    void whitespace()
    {
        while (position_ < text_.size())
        {
            const char c = text_[position_];
            if (c != ' ' && c != '\r' && c != '\n' && c != '\t')
                break;
            ++position_;
        }
    }

    bool take(char value)
    {
        whitespace();
        if (position_ == text_.size() || text_[position_] != value)
            return false;
        ++position_;
        return true;
    }

    void require(char value)
    {
        if (!take(value))
            fail("unexpected token");
    }

    void literal(std::string_view value)
    {
        if (text_.substr(position_, value.size()) != value)
            fail("invalid literal");
        position_ += value.size();
    }

    std::uint32_t hex4()
    {
        std::uint32_t code = 0;
        for (unsigned i = 0; i < 4; ++i)
        {
            if (position_ == text_.size())
                fail("truncated Unicode escape");
            const char c = text_[position_++];
            code <<= 4;
            if (c >= '0' && c <= '9')
                code |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                code |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                code |= static_cast<unsigned>(c - 'A' + 10);
            else
                fail("invalid Unicode escape");
        }
        return code;
    }

    std::string string()
    {
        if (position_ == text_.size() || text_[position_++] != '"')
            fail("expected string");
        std::string out;
        while (position_ < text_.size())
        {
            const unsigned char c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"')
                return out;
            if (c < 0x20)
                fail("unescaped control character");
            if (c == '\\')
            {
                if (position_ == text_.size())
                    fail("truncated escape");
                switch (text_[position_++])
                {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                {
                    std::uint32_t code = hex4();
                    if (code >= 0xd800 && code <= 0xdbff)
                    {
                        if (text_.substr(position_, 2) != "\\u")
                            fail("missing low surrogate");
                        position_ += 2;
                        const std::uint32_t low = hex4();
                        if (low < 0xdc00 || low > 0xdfff)
                            fail("invalid low surrogate");
                        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                    }
                    else if (code >= 0xdc00 && code <= 0xdfff)
                        fail("unpaired low surrogate");
                    append_utf8(out, code);
                    break;
                }
                default: fail("invalid escape");
                }
            }
            else if (c < 0x80)
                out.push_back(static_cast<char>(c));
            else
            {
                unsigned remaining = 0;
                std::uint32_t code = 0;
                std::uint32_t minimum = 0;
                if (c >= 0xc2 && c <= 0xdf) { remaining = 1; code = c & 0x1f; minimum = 0x80; }
                else if (c >= 0xe0 && c <= 0xef) { remaining = 2; code = c & 0x0f; minimum = 0x800; }
                else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; code = c & 0x07; minimum = 0x10000; }
                else fail("invalid UTF-8 lead byte");
                for (unsigned i = 0; i < remaining; ++i)
                {
                    if (position_ == text_.size())
                        fail("truncated UTF-8");
                    const unsigned char continuation = static_cast<unsigned char>(text_[position_++]);
                    if ((continuation & 0xc0) != 0x80)
                        fail("invalid UTF-8 continuation");
                    code = (code << 6) | (continuation & 0x3f);
                }
                if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
                    fail("invalid Unicode codepoint");
                append_utf8(out, code);
            }
        }
        fail("unterminated string");
    }

    Value number()
    {
        const std::size_t begin = position_;
        if (position_ < text_.size() && text_[position_] == '-')
            ++position_;
        if (position_ == text_.size())
            fail("truncated number");
        if (text_[position_] == '0')
            ++position_;
        else
        {
            if (text_[position_] < '1' || text_[position_] > '9')
                fail("invalid number");
            digits();
        }
        if (position_ < text_.size() && text_[position_] == '.')
        {
            ++position_;
            require_digits();
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E'))
        {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
                ++position_;
            require_digits();
        }
        double value = 0;
        const auto result = std::from_chars(text_.data() + begin, text_.data() + position_, value);
        if (result.ec != std::errc{} || result.ptr != text_.data() + position_ || !std::isfinite(value))
            fail("number out of range");
        return Value(value);
    }

    void digits()
    {
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
            ++position_;
    }

    void require_digits()
    {
        const std::size_t begin = position_;
        digits();
        if (position_ == begin)
            fail("expected digit");
    }
};
}

const Value *Value::find(std::string_view key) const
{
    const auto *object = std::get_if<Object>(&data);
    if (object == nullptr)
        return nullptr;
    const auto found = object->find(key);
    return found == object->end() ? nullptr : &found->second;
}

bool parse(std::string_view text, Value &out, std::string *error)
{
    try
    {
        if (text.size() > 4 * 1024 * 1024)
            throw std::runtime_error("JSON exceeds 4 MiB limit");
        Reader reader(text);
        Value parsed = reader.read();
        if (!reader.finished())
            throw std::runtime_error("trailing data after JSON value");
        out = std::move(parsed);
        if (error)
            error->clear();
        return true;
    }
    catch (const std::exception &exception)
    {
        if (error)
            *error = exception.what();
        return false;
    }
}
}
