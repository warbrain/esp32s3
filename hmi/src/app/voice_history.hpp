#pragma once

#include "app/voice_runtime.hpp"
#include "base/project_paths.hpp"
#include "util/json_writer.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace hmi::app::detail
{
inline util::json::Value history_number(double value)
{
    return std::isfinite(value) && value >= 0.0 ? util::json::Value(value) : util::json::Value{};
}

inline void normalize_voice_outcome(VoiceEvent &event)
{
    if (!event.ok) event.cancelled = event.skipped = false;
    else if (event.cancelled) event.skipped = false;
}

inline bool valid_input_delivery(std::string_view value) noexcept
{
    return value == "not_sent" || value == "delivered_unverified" || value == "verified" ||
        value == "mismatch" || value == "unknown";
}

inline bool voice_event_has_input_delivery(const VoiceEvent &event) noexcept
{
    if (!event.speech_text.empty() || event.action == "speak" || !valid_input_delivery(event.input_delivery)) return false;
    return event.action == "text" || event.action.starts_with("text -> ") || event.action.ends_with(" -> text") ||
        event.action.find(" -> text -> ") != std::string::npos;
}

inline const char *voice_event_status(const VoiceEvent &event) noexcept
{
    const bool input = voice_event_has_input_delivery(event);
    if (!event.ok)
    {
        if (input && event.input_delivery == "mismatch") return "文字不符";
        if (input && event.input_delivery == "unknown") return "待确认";
        if (input && event.input_delivery == "not_sent") return "未送出";
        return "未完成";
    }
    if (event.cancelled) return "已停止";
    if (event.skipped) return "已跳过";
    if (input)
    {
        if (event.input_delivery == "verified") return "已核对";
        if (event.input_delivery == "delivered_unverified") return "已送出";
        if (event.input_delivery == "mismatch") return "文字不符";
        if (event.input_delivery == "unknown") return "待确认";
        if (event.input_delivery == "not_sent") return "未送出";
    }
    return "已完成";
}

inline util::json::Value::Object voice_event_fields(const VoiceEvent &event, std::string_view run_id)
{
    using util::json::Value;
    Value::Object fields{{"event", Value("voice_result")}, {"schema", Value(1.0)}, {"run_id", Value(std::string(run_id))},
        {"request_id", Value(std::to_string(event.request_id))}, {"event_id", Value(std::to_string(event.sequence))},
        {"recorded_at_ms", Value(static_cast<double>(event.recorded_at_ms))},
        {"request_at_ms", Value(static_cast<double>(event.at_ms))},
        {"transcript", Value(event.transcript)}, {"speech_text", Value(event.speech_text)},
        {"rule", Value(event.rule)}, {"action", Value(event.action)}, {"detail", Value(event.detail)},
        {"result", Value(!event.ok ? "failed" : event.cancelled ? "cancelled" : event.skipped ? "skipped" : "success")},
        {"confidence", Value(std::isfinite(event.confidence) ? static_cast<double>(event.confidence) : -1.0)},
        {"latency_ms", history_number(event.latency_ms)}, {"asr_ms", history_number(event.asr_ms)},
        {"tts_ms", history_number(event.tts_ms)}, {"queue_ms", history_number(event.queue_ms)},
        {"rule_ms", history_number(event.rule_ms)}, {"action_ms", history_number(event.action_ms)},
        {"stop_reason", Value(event.stop_reason)}};
    if (valid_input_delivery(event.input_delivery)) fields.emplace("input_delivery", Value(event.input_delivery));
    return fields;
}

inline bool read_voice_event(std::string_view line, VoiceEvent &out)
{
    constexpr std::string_view marker = "event=voice_result ";
    const auto offset = line.find(marker);
    if (offset == std::string_view::npos || line.size() - offset > 128 * 1024) return false;
    util::json::Value value;
    if (!util::json::parse(line.substr(offset + marker.size()), value)) return false;
    const auto string = [&value](const char *key) -> const std::string * {
        const auto *field = value.find(key);
        return field ? std::get_if<std::string>(&field->data) : nullptr;
    };
    const auto number = [&value](const char *key, double fallback) {
        const auto *field = value.find(key);
        const auto *result = field ? std::get_if<double>(&field->data) : nullptr;
        return result && std::isfinite(*result) ? *result : fallback;
    };
    const auto id = [&string](const char *key, std::uint64_t &result) {
        const auto *text = string(key);
        if (!text || text->empty()) return false;
        const auto parsed = std::from_chars(text->data(), text->data() + text->size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == text->data() + text->size();
    };
    const auto *kind = string("event");
    const auto *result = string("result");
    const auto *text = string("transcript");
    const auto *speech = string("speech_text");
    if (!kind || *kind != "voice_result" || number("schema", 0.0) != 1.0 || !result || !text || !speech ||
        text->size() > 16384 || speech->size() > 16384 ||
        (*result != "success" && *result != "failed" && *result != "cancelled" && *result != "skipped")) return false;
    VoiceEvent event;
    if (!id("event_id", event.sequence) || !id("request_id", event.request_id)) return false;
    const double recorded = number("recorded_at_ms", -1.0);
    if (recorded <= 0.0 || recorded > 253402271999999.0 || std::floor(recorded) != recorded) return false;
    event.recorded_at_ms = static_cast<std::int64_t>(recorded);
    event.transcript = *text;
    event.speech_text = *speech;
    const auto copy = [&string](const char *key, std::string &target) {
        if (const auto *source = string(key); source && source->size() <= 16384) target = *source;
    };
    copy("rule", event.rule); copy("action", event.action); copy("detail", event.detail);
    copy("stop_reason", event.stop_reason);
    if (const auto *delivery = string("input_delivery"); delivery && valid_input_delivery(*delivery))
        event.input_delivery = *delivery;
    event.ok = *result != "failed";
    event.cancelled = *result == "cancelled";
    event.skipped = *result == "skipped";
    event.confidence = static_cast<float>(std::clamp(number("confidence", -1.0), -1.0, 1.0));
    event.latency_ms = number("latency_ms", -1.0);
    if (event.latency_ms < 0.0) event.latency_ms = -1.0;
    event.asr_ms = number("asr_ms", -1.0);
    event.tts_ms = number("tts_ms", -1.0);
    event.queue_ms = number("queue_ms", -1.0);
    event.rule_ms = number("rule_ms", -1.0);
    event.action_ms = number("action_ms", -1.0);
    out = std::move(event);
    return true;
}

// A bounded startup tail scan; old diagnostic lines never become executable actions.
inline std::size_t restore_voice_history(const std::filesystem::path &file, std::deque<VoiceEvent> &events,
                                        std::size_t backup_count = 3)
{
    constexpr std::streamoff tail_limit = 2 * 1024 * 1024;
    std::size_t restored = 0;
    const auto base_directory = base::process_directory();
    for (int backup = static_cast<int>(std::min<std::size_t>(backup_count, 100)); backup >= 0; --backup)
    {
        auto candidate = file;
        if (backup != 0)
            candidate = file.parent_path() / (file.stem().wstring() + L"." + std::to_wstring(backup) + file.extension().wstring());
        std::filesystem::path checked;
        if (!base::resolve_project_path(candidate, base_directory, checked)) continue;
        std::ifstream input(checked, std::ios::binary);
        if (!input) continue;
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        if (size < 0) continue;
        const auto start = std::max(std::streamoff{0}, static_cast<std::streamoff>(size) - tail_limit);
        input.seekg(start);
        std::string tail(static_cast<std::size_t>(static_cast<std::streamoff>(size) - start), '\0');
        input.read(tail.data(), static_cast<std::streamsize>(tail.size()));
        tail.resize(static_cast<std::size_t>(input.gcount()));
        std::istringstream lines(std::move(tail));
        std::string line;
        if (start != 0) std::getline(lines, line);
        while (std::getline(lines, line))
        {
            VoiceEvent event;
            if (!read_voice_event(line, event)) continue;
            events.push_back(std::move(event));
            if (events.size() > 20) events.pop_front();
            ++restored;
        }
    }
    return restored;
}
}
