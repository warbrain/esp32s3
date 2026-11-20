#include "check.hpp"
#include "util/json_reader.hpp"
#include "util/json_writer.hpp"

#include <limits>
#include <stdexcept>

HMI_TEST(json_nested_unicode_roundtrip)
{
    using namespace hmi::util::json;
    Value value;
    std::string error;
    CHECK(parse(R"({"text":"\u4f60\u597d\n\t\"\\\ud83d\ude00","array":[true,false,null,-12.5e2],"extra":{"skip":[1,2]}})", value, &error));
    CHECK(value.find("text") != nullptr);
    if (value.find("text"))
        CHECK(std::get<std::string>(value.find("text")->data) == "你好\n\t\"\\😀");
    const std::string serialized = stringify(value);
    Value roundtrip;
    CHECK(parse(serialized, roundtrip, &error));
    CHECK(stringify(roundtrip) == serialized);
}

HMI_TEST(json_rejects_truncation_trailing_and_invalid_numbers)
{
    using namespace hmi::util::json;
    constexpr const char *invalid[] = {
        "", "{", "[1,]", "{\"a\":1,}", "true false", "01", "-01", "+1", "1.", "1e", "1e+",
        "NaN", "Infinity", "1e999", "1e-9999", "{\"a\":1,\"a\":2}", "\"unterminated", "\"bad\\x\"",
        "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0000\"", "\"\\uZZZZ\"", "\"\n\""};
    for (const char *text : invalid)
    {
        Value value(std::string("preserved"));
        std::string error;
        CHECK(!parse(text, value, &error));
        CHECK(!error.empty());
        CHECK(std::get<std::string>(value.data) == "preserved");
    }
    Value value;
    CHECK(!parse(std::string("\"") + '\xc0' + '\x80' + '"', value));
    CHECK(!parse(std::string("\"") + '\xed' + '\xa0' + '\x80' + '"', value));
    CHECK(!parse(std::string(70, '[') + "0" + std::string(70, ']'), value));
}

HMI_TEST(json_writer_rejects_nonfinite)
{
    bool rejected = false;
    try
    {
        static_cast<void>(hmi::util::json::stringify(hmi::util::json::Value(std::numeric_limits<double>::infinity())));
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    CHECK(rejected);
}
