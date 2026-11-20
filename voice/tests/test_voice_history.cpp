#include "check.hpp"
#include "app/voice_history.hpp"
#include "audio/playback_diagnostics.hpp"

#include <chrono>
#include <fstream>

namespace
{
struct HistoryFiles
{
    std::filesystem::path file = hmi::base::process_directory() /
        ("hmi-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    std::filesystem::path backup = file.parent_path() / (file.stem().string() + ".1.log");
    ~HistoryFiles()
    {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        std::filesystem::remove(backup, ignored);
    }
};
}

HMI_TEST(voice_history_roundtrip_keeps_text_timing_and_request_identity)
{
    hmi::app::VoiceEvent original;
    original.at_ms = 123;
    original.recorded_at_ms = 1793616000123;
    original.request_id = 9007199254740993ULL;
    original.sequence = 42;
    original.transcript = "原句：\"你好\"\n第二行";
    original.speech_text = "朗读这句。";
    original.rule = "转写朗读";
    original.action = "speak";
    original.detail = "已停止";
    original.ok = original.cancelled = true;
    original.asr_ms = 347.25;
    original.tts_ms = 0.0;
    const auto payload = hmi::util::json::stringify(hmi::util::json::Value(
        hmi::app::detail::voice_event_fields(original, "test-run")));
    hmi::app::VoiceEvent restored;
    CHECK(hmi::app::detail::read_voice_event("prefix [history] event=voice_result " + payload, restored));
    CHECK(restored.request_id == original.request_id && restored.sequence == original.sequence);
    CHECK(restored.recorded_at_ms == original.recorded_at_ms);
    CHECK(restored.transcript == original.transcript && restored.speech_text == original.speech_text);
    CHECK(restored.asr_ms == 347.25 && restored.tts_ms == 0.0);
    CHECK(restored.ok && restored.cancelled && !restored.skipped);
    CHECK(restored.at_ms == 0);
}

HMI_TEST(voice_history_rejects_truncated_or_unrelated_entries_transactionally)
{
    hmi::app::VoiceEvent event;
    event.transcript = "保留原值";
    for (const auto *line : {"[perf] cpu=1", "event=voice_result {", "event=voice_result {}",
         "event=voice_result {\"event\":\"voice_result\",\"schema\":2}"})
    {
        CHECK(!hmi::app::detail::read_voice_event(line, event));
        CHECK(event.transcript == "保留原值");
    }
}

HMI_TEST(voice_history_roundtrips_all_input_delivery_states_without_claiming_verification)
{
    struct DeliveryCase
    {
        const char *delivery;
        const char *status;
        bool ok;
    };
    const DeliveryCase cases[] = {
        {"verified", "已核对", true},
        {"delivered_unverified", "已送出", true},
        {"mismatch", "文字不符", false},
        {"unknown", "待确认", false},
        {"not_sent", "未送出", false}
    };
    for (const auto &item : cases)
    {
        hmi::app::VoiceEvent original;
        original.recorded_at_ms = 1793616000123;
        original.request_id = 87;
        original.sequence = 91;
        original.transcript = "实际输入的文字";
        original.action = "text";
        original.input_delivery = item.delivery;
        original.ok = item.ok;
        const hmi::util::json::Value fields(hmi::app::detail::voice_event_fields(original, "delivery-run"));
        const auto *delivery = fields.find("input_delivery");
        CHECK(delivery != nullptr);
        if (delivery) CHECK(std::get<std::string>(delivery->data) == item.delivery);
        CHECK(std::get<std::string>(fields.find("result")->data) == (item.ok ? "success" : "failed"));
        hmi::app::VoiceEvent restored;
        CHECK(hmi::app::detail::read_voice_event("event=voice_result " + hmi::util::json::stringify(fields), restored));
        CHECK(restored.input_delivery == item.delivery);
        CHECK(restored.transcript == original.transcript && restored.request_id == original.request_id);
        CHECK(restored.ok == item.ok);
        CHECK(std::string_view(hmi::app::detail::voice_event_status(restored)) == item.status);
        if (restored.input_delivery != "verified")
            CHECK(std::string_view(hmi::app::detail::voice_event_status(restored)) != "已核对");
    }
}

HMI_TEST(voice_history_reads_legacy_schema_without_input_delivery)
{
    constexpr std::string_view legacy =
        "event=voice_result {\"event\":\"voice_result\",\"schema\":1,\"request_id\":\"87\",\"event_id\":\"91\","
        "\"recorded_at_ms\":1793616000123,\"transcript\":\"旧记录\",\"speech_text\":\"\",\"action\":\"text\",\"result\":\"success\"}";
    hmi::app::VoiceEvent event;
    event.input_delivery = "verified";
    CHECK(hmi::app::detail::read_voice_event(legacy, event));
    CHECK(event.input_delivery.empty());
    CHECK(event.request_id == 87 && event.sequence == 91 && event.transcript == "旧记录");
    CHECK(std::string_view(hmi::app::detail::voice_event_status(event)) == "已完成");
    const hmi::util::json::Value fields(hmi::app::detail::voice_event_fields(event, "legacy-run"));
    CHECK(fields.find("input_delivery") == nullptr);
    event.ok = false;
    CHECK(std::string_view(hmi::app::detail::voice_event_status(event)) == "未完成");
}

HMI_TEST(voice_history_ignores_invalid_optional_delivery_without_losing_the_record)
{
    using hmi::util::json::Value;
    hmi::app::VoiceEvent original;
    original.recorded_at_ms = 1793616000123;
    original.request_id = 87;
    original.sequence = 91;
    original.transcript = "不能因新字段丢失";
    original.action = "text";
    original.ok = true;
    const Value invalid[] = {Value("future_state"), Value(""), Value("VERIFIED"), Value(true), Value(1.0), Value{}};
    for (const auto &item : invalid)
    {
        auto fields = hmi::app::detail::voice_event_fields(original, "optional-run");
        fields.emplace("input_delivery", item);
        hmi::app::VoiceEvent restored;
        restored.input_delivery = "verified";
        CHECK(hmi::app::detail::read_voice_event("event=voice_result " + hmi::util::json::stringify(Value(fields)), restored));
        CHECK(restored.input_delivery.empty());
        CHECK(restored.ok && restored.request_id == original.request_id && restored.sequence == original.sequence);
        CHECK(restored.transcript == original.transcript);
        CHECK(std::string_view(hmi::app::detail::voice_event_status(restored)) == "已完成");
    }
    original.input_delivery = "future_state";
    const Value fields(hmi::app::detail::voice_event_fields(original, "optional-run"));
    CHECK(fields.find("input_delivery") == nullptr);
    CHECK(std::string_view(hmi::app::detail::voice_event_status(original)) == "已完成");
    original.ok = false;
    CHECK(std::string_view(hmi::app::detail::voice_event_status(original)) == "未完成");
}

HMI_TEST(voice_history_readback_rows_do_not_borrow_text_delivery_status)
{
    hmi::app::VoiceEvent event;
    event.ok = true;
    for (const auto *delivery : {"verified", "delivered_unverified", "mismatch", "unknown", "not_sent"})
    {
        event.input_delivery = delivery;
        event.action = "speak";
        event.speech_text.clear();
        CHECK(std::string_view(hmi::app::detail::voice_event_status(event)) == "已完成");
        event.action = "text -> speak";
        event.speech_text = "实际朗读内容";
        CHECK(std::string_view(hmi::app::detail::voice_event_status(event)) == "已完成");
        event.ok = false;
        CHECK(std::string_view(hmi::app::detail::voice_event_status(event)) == "未完成");
        event.ok = true;
    }
    event.action = "launch";
    event.speech_text.clear();
    event.input_delivery = "verified";
    CHECK(std::string_view(hmi::app::detail::voice_event_status(event)) == "已完成");
}

HMI_TEST(voice_history_cancellation_skip_and_transport_failure_take_precedence_over_verification)
{
    hmi::app::VoiceEvent event;
    event.action = "text";
    event.ok = true;
    for (const auto *delivery : {"verified", "delivered_unverified", "mismatch", "unknown", "not_sent"})
    {
        event.input_delivery = delivery;
        event.cancelled = event.skipped = true;
        CHECK(std::string_view(hmi::app::detail::voice_event_status(event)) == "已停止");
        event.cancelled = false;
        CHECK(std::string_view(hmi::app::detail::voice_event_status(event)) == "已跳过");
        event.skipped = false;
    }
    for (const auto *delivery : {"verified", "delivered_unverified"})
    {
        event.input_delivery = delivery;
        event.ok = false;
        event.cancelled = event.skipped = true;
        CHECK(std::string_view(hmi::app::detail::voice_event_status(event)) == "未完成");
        hmi::app::detail::normalize_voice_outcome(event);
        CHECK(!event.ok && !event.cancelled && !event.skipped);
        CHECK(std::string_view(hmi::app::detail::voice_event_status(event)) == "未完成");
    }
}

HMI_TEST(voice_history_reads_rotation_in_order_and_keeps_twenty_results)
{
    HistoryFiles files;
    for (int part = 0; part < 2; ++part)
    {
        std::ofstream output(part == 0 ? files.backup : files.file, std::ios::binary);
        CHECK(output.good());
        output << "[perf] unrelated diagnostic\n";
        for (int index = part == 0 ? 1 : 11; index <= (part == 0 ? 10 : 30); ++index)
        {
            hmi::app::VoiceEvent event;
            event.request_id = event.sequence = static_cast<std::uint64_t>(index);
            event.recorded_at_ms = 1793616000000 + index;
            event.transcript = "记录" + std::to_string(index);
            event.ok = true;
            output << "[history] event=voice_result " << hmi::util::json::stringify(hmi::util::json::Value(
                hmi::app::detail::voice_event_fields(event, "fixture"))) << '\n';
        }
        output << "[history] event=voice_result {truncated\n";
    }
    std::deque<hmi::app::VoiceEvent> restored;
    CHECK(hmi::app::detail::restore_voice_history(files.file, restored, 1) == 30);
    CHECK(restored.size() == 20);
    CHECK(restored.front().request_id == 11 && restored.back().request_id == 30);
    CHECK(restored.back().transcript == "记录30");
}

HMI_TEST(voice_history_preserves_unknown_latency_and_canonical_failure_after_cancellation)
{
    hmi::app::VoiceEvent event;
    event.recorded_at_ms = 1793616000123;
    event.request_id = 7;
    event.sequence = 21;
    event.transcript = "先停止朗读，再执行失败的动作";
    event.action = "speak -> launch";
    event.ok = false;
    event.cancelled = true;
    event.skipped = true;
    event.latency_ms = -1.0;
    event.stop_reason = "action_failed";
    hmi::app::detail::normalize_voice_outcome(event);
    CHECK(!event.cancelled && !event.skipped && !event.ok);
    const auto payload = hmi::util::json::stringify(hmi::util::json::Value(
        hmi::app::detail::voice_event_fields(event, "test-run")));
    hmi::app::VoiceEvent restored;
    CHECK(hmi::app::detail::read_voice_event("[history] event=voice_result " + payload, restored));
    CHECK(restored.latency_ms < 0.0 && !restored.cancelled && !restored.skipped && !restored.ok);
    CHECK(restored.stop_reason == "action_failed");
    event.ok = event.cancelled = true;
    event.skipped = true;
    hmi::app::detail::normalize_voice_outcome(event);
    CHECK(event.ok && event.cancelled && !event.skipped);
}

HMI_TEST(playback_diagnostics_distinguishes_pending_unknown_and_final_device_measurements)
{
    hmi::audio::PlaybackStatistics playback;
    CHECK(std::holds_alternative<std::nullptr_t>(hmi::audio::detail::playback_fields(playback).data));
    playback.finalized = true;
    playback.finish_reason = "producer_failed";
    const auto no_device = hmi::audio::detail::playback_fields(playback);
    CHECK(std::holds_alternative<std::nullptr_t>(no_device.find("consumed_frames")->data));
    CHECK(std::holds_alternative<std::nullptr_t>(no_device.find("sample_rate")->data));
    CHECK(std::get<double>(no_device.find("submitted_frames")->data) == 0.0);
    playback.client_started = playback.padding_available = true;
    playback.sample_rate = 24000;
    playback.channels = 1;
    playback.submitted_frames = 7953;
    playback.consumed_frames = 5793;
    playback.finish_reason = "interrupted";
    const auto final = hmi::audio::detail::playback_fields(playback);
    CHECK(std::get<bool>(final.find("client_started")->data));
    CHECK(std::get<double>(final.find("submitted_frames")->data) == 7953.0);
    CHECK(std::get<double>(final.find("consumed_frames")->data) == 5793.0);
    CHECK(std::get<std::string>(final.find("finish_reason")->data) == "interrupted");
}
