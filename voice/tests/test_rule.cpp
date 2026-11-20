#include "check.hpp"
#include "intent/rule.hpp"
#include "intent/rule_io.hpp"

#include <filesystem>
#include <limits>

namespace
{
hmi::intent::RuleSet matching_rules()
{
    hmi::intent::Rule rule;
    rule.id = "open";
    rule.trigger.pattern = "open";
    rule.cooldown_ms = 800;
    hmi::intent::RuleSet rules;
    rules.rules.push_back(rule);
    return rules;
}
}

HMI_TEST(normalize_fullwidth_and_ascii_without_corrupting_chinese)
{
    CHECK(hmi::intent::normalize("　ＡＢＣ１２３！　") == "abc123!");
    CHECK(hmi::intent::normalize(" \tOpen 计算器\r\n") == "open 计算器");
    CHECK(hmi::intent::normalize("　 \n").empty());
}

HMI_TEST(rule_first_match_and_cooldown_clock_origin)
{
    auto rules = matching_rules();
    std::string phrase;
    CHECK(hmi::intent::match(rules, "please OPEN calculator", 0.8f, 0, &phrase) == &rules.rules.front());
    CHECK(phrase == "open");
    CHECK(hmi::intent::match(rules, "open", 0.8f, 799) == nullptr);
    CHECK(hmi::intent::match(rules, "open", 0.8f, 800) == &rules.rules.front());
    CHECK(hmi::intent::match(rules, "open", 0.8f, 400) == nullptr);
}

HMI_TEST(rule_priority_disabled_and_confidence_policy)
{
    auto rules = matching_rules();
    rules.rules.push_back(rules.rules.front());
    rules.rules.front().enabled = false;
    CHECK(hmi::intent::match(rules, "open", 0.9f, 50) == &rules.rules.back());
    CHECK(hmi::intent::match(rules, "open", 0.5f, 1000) == nullptr);
    CHECK(hmi::intent::match(rules, "open", -1.0f, 1000) == nullptr);
    rules.rules.back().trigger.min_confidence = 0;
    CHECK(hmi::intent::match(rules, "open", -1.0f, 1000) == &rules.rules.back());
    CHECK(hmi::intent::match(rules, "open", std::numeric_limits<float>::quiet_NaN(), 2000) == nullptr);
    CHECK(hmi::intent::match(rules, "open", 1.01f, 2000) == nullptr);
}

HMI_TEST(regex_preserves_escape_case_and_handles_invalid_expression)
{
    auto rules = matching_rules();
    rules.rules.front().trigger.kind = hmi::intent::TriggerKind::Regex;
    rules.rules.front().trigger.pattern = R"(^\D+$)";
    CHECK(hmi::intent::match(rules, "OPEN", 1.0f, 10) != nullptr);
    CHECK(hmi::intent::match(rules, "123", 1.0f, 1000) == nullptr);
    rules.rules.front().trigger.pattern = "[";
    CHECK(hmi::intent::match(rules, "open", 1.0f, 1000) == nullptr);
}

HMI_TEST(rule_json_roundtrip_and_transactional_failure)
{
    const auto defaults = hmi::intent::default_rules();
    const std::string text = hmi::intent::serialize_rules(defaults);
    hmi::intent::RuleSet parsed;
    std::string error;
    CHECK(hmi::intent::parse_rules(text, parsed, &error));
    CHECK(error.empty());
    CHECK(hmi::intent::serialize_rules(parsed) == text);
    CHECK(parsed.rules.size() == 4);
    CHECK(!hmi::intent::parse_rules("{", parsed, &error));
    CHECK(!error.empty());
    CHECK(hmi::intent::serialize_rules(parsed) == text);
    CHECK(!hmi::intent::parse_rules(R"({"rules":[{"id":"x","trigger":{"kind":"phrase","pattern":"x","min_confidence":2},"actions":[]}]})", parsed, &error));
    CHECK(!hmi::intent::parse_rules(R"({"rules":[{"id":"x","trigger":{"kind":"regex","pattern":"["},"actions":[{"kind":"wait","params":{"ms":"0"}}]}]})", parsed, &error));
    CHECK(hmi::intent::serialize_rules(parsed) == text);
}

HMI_TEST(shipped_rules_equal_builtin_rules_and_bad_load_preserves_state)
{
    const auto file = std::filesystem::path(__FILE__).parent_path().parent_path() / "rules" / "voice.json";
    hmi::intent::RuleSet loaded;
    std::string error;
    CHECK(hmi::intent::load_rules(file.string(), loaded, &error));
    CHECK(hmi::intent::serialize_rules(loaded) == hmi::intent::serialize_rules(hmi::intent::default_rules()));
    const std::string old_path = loaded.source_path;
    CHECK(!hmi::intent::load_rules(file.string() + ".missing", loaded, &error));
    CHECK(loaded.source_path == old_path);
    CHECK(loaded.rules.size() == 4);
}
