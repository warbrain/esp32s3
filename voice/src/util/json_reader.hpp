#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace hmi::util::json
{
struct Value
{
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data = nullptr;

    Value() = default;
    explicit Value(bool value) : data(value) {}
    explicit Value(double value) : data(value) {}
    explicit Value(std::string value) : data(std::move(value)) {}
    explicit Value(const char *value) : data(std::string(value)) {}
    explicit Value(Array value) : data(std::move(value)) {}
    explicit Value(Object value) : data(std::move(value)) {}

    const Value *find(std::string_view key) const;
};

// Rejects malformed Unicode, duplicate keys, non-finite numbers and trailing data.
// Parsing is transactional: out is unchanged on failure.
bool parse(std::string_view text, Value &out, std::string *error = nullptr);
}
