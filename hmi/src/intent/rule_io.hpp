#pragma once

#include "intent/rule.hpp"

namespace hmi::intent
{
// Transactional: a failed parse or load leaves the existing rules untouched.
bool parse_rules(std::string_view text, RuleSet &out, std::string *error = nullptr);
bool load_rules(const std::string &path, RuleSet &out, std::string *error = nullptr);
std::string serialize_rules(const RuleSet &rules);
RuleSet default_rules();
}
