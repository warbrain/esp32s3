#include "intent/rule_io.hpp"

#include "util/json_reader.hpp"
#include "util/json_writer.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>

namespace hmi::intent
{
namespace
{
using util::json::Value;

const Value &required(const Value &object, std::string_view key)
{
    if (const Value *value = object.find(key))
        return *value;
    throw std::runtime_error("missing field: " + std::string(key));
}

std::string string(const Value &value, std::string_view label)
{
    const auto *text = std::get_if<std::string>(&value.data);
    if (!text || text->find('\0') != std::string::npos)
        throw std::runtime_error(std::string(label) + " must be a string without NUL");
    return *text;
}

double number(const Value &value, std::string_view label, double low, double high)
{
    const auto *result = std::get_if<double>(&value.data);
    if (!result || !std::isfinite(*result) || *result < low || *result > high)
        throw std::runtime_error(std::string(label) + " is outside its allowed range");
    return *result;
}

const Value::Object &object(const Value &value, std::string_view label)
{
    const auto *result = std::get_if<Value::Object>(&value.data);
    if (!result)
        throw std::runtime_error(std::string(label) + " must be an object");
    return *result;
}

const Value::Array &array(const Value &value, std::string_view label)
{
    const auto *result = std::get_if<Value::Array>(&value.data);
    if (!result)
        throw std::runtime_error(std::string(label) + " must be an array");
    return *result;
}

const char *kind_name(TriggerKind kind)
{
    switch (kind)
    {
    case TriggerKind::Phrase: return "phrase";
    case TriggerKind::Regex: return "regex";
    case TriggerKind::Hotkey: return "hotkey";
    }
    throw std::runtime_error("invalid trigger kind");
}
}

bool parse_rules(std::string_view text, RuleSet &out, std::string *error)
{
    try
    {
        Value root;
        std::string parse_error;
        if (!util::json::parse(text, root, &parse_error))
            throw std::runtime_error(parse_error);
        object(root, "document");
        if (const Value *schema = root.find("schema"); schema && number(*schema, "schema", 1, 1) != 1)
            throw std::runtime_error("unsupported rule schema");
        const auto &items = array(required(root, "rules"), "rules");
        if (items.size() > 256)
            throw std::runtime_error("at most 256 rules are supported");
        RuleSet parsed;
        parsed.source_path = out.source_path;
        std::set<std::string> ids;
        for (const Value &item : items)
        {
            object(item, "rule");
            Rule rule;
            rule.id = string(required(item, "id"), "id");
            if (rule.id.empty() || !ids.insert(rule.id).second)
                throw std::runtime_error("rule ids must be nonempty and unique");
            rule.name = item.find("name") ? string(*item.find("name"), "name") : rule.id;
            if (const Value *enabled = item.find("enabled"))
            {
                const auto *flag = std::get_if<bool>(&enabled->data);
                if (!flag)
                    throw std::runtime_error("enabled must be boolean");
                rule.enabled = *flag;
            }
            if (const Value *cooldown = item.find("cooldown_ms"))
            {
                const double milliseconds = number(*cooldown, "cooldown_ms", 0, 86400000);
                if (milliseconds != std::floor(milliseconds))
                    throw std::runtime_error("cooldown_ms must be an integer");
                rule.cooldown_ms = static_cast<int>(milliseconds);
            }
            const Value &trigger = required(item, "trigger");
            object(trigger, "trigger");
            const std::string kind = string(required(trigger, "kind"), "trigger.kind");
            if (kind == "phrase") rule.trigger.kind = TriggerKind::Phrase;
            else if (kind == "regex") rule.trigger.kind = TriggerKind::Regex;
            else if (kind == "hotkey") rule.trigger.kind = TriggerKind::Hotkey;
            else throw std::runtime_error("unsupported trigger kind: " + kind);
            rule.trigger.pattern = string(required(trigger, "pattern"), "trigger.pattern");
            if (normalize(rule.trigger.pattern).empty() || rule.trigger.pattern.size() > 1024)
                throw std::runtime_error("trigger pattern must contain 1..1024 bytes");
            if (const Value *threshold = trigger.find("min_confidence"))
                rule.trigger.min_confidence = static_cast<float>(number(*threshold, "min_confidence", 0, 1));
            if (rule.trigger.kind == TriggerKind::Regex)
                static_cast<void>(std::regex(rule.trigger.pattern, std::regex::ECMAScript | std::regex::icase));
            const auto &actions = array(required(item, "actions"), "actions");
            if (actions.empty() || actions.size() > 64)
                throw std::runtime_error("each rule needs 1..64 actions");
            for (const Value &entry : actions)
            {
                object(entry, "action");
                Action action;
                action.kind = string(required(entry, "kind"), "action.kind");
                if (action.kind != "launch" && action.kind != "key" && action.kind != "text" &&
                    action.kind != "speak" && action.kind != "wait")
                    throw std::runtime_error("unsupported action kind: " + action.kind);
                if (const Value *params = entry.find("params"))
                    for (const auto &[key, value] : object(*params, "params"))
                        action.params.emplace(key, string(value, "action parameter"));
                rule.actions.push_back(std::move(action));
            }
            parsed.rules.push_back(std::move(rule));
        }
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

bool load_rules(const std::string &path, RuleSet &out, std::string *error)
{
    try
    {
        const std::filesystem::path file_path(std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size()));
        std::error_code code;
        const auto size = std::filesystem::file_size(file_path, code);
        if (code || size > 4 * 1024 * 1024)
            throw std::runtime_error("cannot read rule file or file exceeds 4 MiB: " + path);
        std::ifstream file(file_path, std::ios::binary);
        if (!file)
            throw std::runtime_error("cannot open rule file: " + path);
        const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (file.bad())
            throw std::runtime_error("cannot finish reading rule file: " + path);
        RuleSet loaded;
        if (!parse_rules(contents, loaded, error))
            return false;
        loaded.source_path = path;
        out = std::move(loaded);
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

std::string serialize_rules(const RuleSet &rules)
{
    Value::Array items;
    for (const Rule &rule : rules.rules)
    {
        Value::Array actions;
        for (const Action &action : rule.actions)
        {
            Value::Object params;
            for (const auto &[key, value] : action.params)
                params.emplace(key, Value(value));
            actions.emplace_back(Value::Object{{"kind", Value(action.kind)}, {"params", Value(std::move(params))}});
        }
        items.emplace_back(Value::Object{
            {"id", Value(rule.id)}, {"name", Value(rule.name)}, {"enabled", Value(rule.enabled)},
            {"cooldown_ms", Value(static_cast<double>(rule.cooldown_ms))},
            {"trigger", Value(Value::Object{{"kind", Value(kind_name(rule.trigger.kind))},
                {"pattern", Value(rule.trigger.pattern)}, {"min_confidence", Value(static_cast<double>(rule.trigger.min_confidence))}})},
            {"actions", Value(std::move(actions))}});
    }
    return util::json::stringify(Value(Value::Object{{"schema", Value(1.0)}, {"rules", Value(std::move(items))}}));
}

RuleSet default_rules()
{
    RuleSet rules;
    const auto add = [&rules](std::string id, std::string name, std::string phrase, std::vector<Action> actions) {
        Rule rule;
        rule.id = std::move(id);
        rule.name = std::move(name);
        rule.trigger.pattern = std::move(phrase);
        // Qwen3 does not report a calibrated confidence; zero explicitly opts
        // these push-to-talk rules into accepting an unknown confidence.
        rule.trigger.min_confidence = 0.0f;
        rule.actions = std::move(actions);
        rules.rules.push_back(std::move(rule));
    };
    add("calculator", "启动计算器", "打开计算器", {{"launch", {{"path", "calc.exe"}}}});
    add("return", "按下回车", "按下回车", {{"key", {{"chord", "Return"}}}});
    add("clear-line", "清空一行", "清空一行",
        {{"key", {{"chord", "Ctrl+A"}}}, {"key", {{"chord", "Delete"}}}});
    add("speak", "朗读", "朗读", {{"speak", {{"text", "你好，语音操控已就绪。"}}}});
    return rules;
}
}
