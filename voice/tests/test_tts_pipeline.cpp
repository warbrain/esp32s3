#include "check.hpp"
#include "engine/tts_pipeline.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <stdexcept>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace hmi::engine::detail;
using hmi::audio::detail::ReadAudio;
using hmi::audio::detail::StreamRead;

std::string concatenate(const std::vector<std::string> &segments)
{
    std::string result;
    for (const auto &segment : segments) result += segment;
    return result;
}

bool utf8(const std::string &text)
{
    return !text.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0) > 0;
}

bool wait_until(const std::function<bool()> &condition)
{
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!condition())
    {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

hmi::audio::AudioBuffer pcm(int index)
{
    hmi::audio::AudioBuffer result;
    result.samples = {static_cast<float>(index)};
    return result;
}

bool consume(const ReadAudio &read, std::string *error, const std::function<bool()> &cancelled,
             const std::function<void(const hmi::audio::AudioBuffer &)> &on_chunk = {})
{
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    for (;;)
    {
        if (cancelled && cancelled())
        {
            if (error) *error = "playback interrupted";
            return false;
        }
        hmi::audio::AudioBuffer audio;
        switch (read(audio, error))
        {
        case StreamRead::Ready:
            if (on_chunk) on_chunk(audio);
            break;
        case StreamRead::Finished: return true;
        case StreamRead::Failed: return false;
        case StreamRead::Pending:
            if (Clock::now() >= deadline)
            {
                if (error) *error = "test stream deadline exceeded";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            break;
        }
    }
}
}

HMI_TEST(tts_segments_preserve_utf8_and_prefer_natural_sentences)
{
    const std::string text = "你好，欢迎使用本地语音助手。现在开始分段朗读，减少等待。\n最后一段！";
    const auto segments = split_tts_text(text);
    CHECK(segments.size() == 5);
    CHECK(concatenate(segments) == text);
    for (const auto &segment : segments) CHECK(utf8(segment));
    CHECK(segments.front() == "你好，");
    CHECK(segments[1] == "欢迎使用本地语音助手。");
}

HMI_TEST(tts_segments_start_at_a_short_natural_opening_without_tiny_tails)
{
    const std::string greeting = "你好，语音操控已就绪。";
    const auto early = split_tts_text(greeting);
    CHECK((early == std::vector<std::string>{"你好，", "语音操控已就绪。"}));
    CHECK(concatenate(early) == greeting);
    CHECK((split_tts_text("提示：马上开始播放。") ==
           std::vector<std::string>{"提示：", "马上开始播放。"}));
    CHECK((split_tts_text("耳机、扬声器均可播放。") ==
           std::vector<std::string>{"耳机、", "扬声器均可播放。"}));
    CHECK((split_tts_text("你好，开始。") == std::vector<std::string>{"你好，开始。"}));
    CHECK((split_tts_text("你好，已就绪。") == std::vector<std::string>{"你好，已就绪。"}));
    CHECK((split_tts_text("前面很多很多文字哦，已就绪。") ==
           std::vector<std::string>{"前面很多很多文字哦，已就绪。"}));
    CHECK((split_tts_text("好，语音操控已就绪。") == std::vector<std::string>{"好，语音操控已就绪。"}));
    CHECK((split_tts_text("Hi there.") == std::vector<std::string>{"Hi there."}));
    CHECK((split_tts_text("Hello world.") == std::vector<std::string>{"Hello world."}));
    const std::string padded = "  “你好，”语音操控已就绪。";
    const auto preserved = split_tts_text(padded);
    CHECK(preserved.size() == 2 && preserved.front() == "  “你好，”");
    CHECK(concatenate(preserved) == padded);
    for (const auto &segment : preserved) CHECK(utf8(segment));
}

HMI_TEST(tts_segments_keep_long_travel_sentence_clauses_intact)
{
    const std::vector<std::string> clauses{
        "咱们周五晚上在太原南过夜，", "然后是第二天去临汾，", "并且是第二天租车，",
        "时间比较紧张，", "因为没有直达的火车，", "或者是时间点就没那么好。"};
    const auto text = concatenate(clauses);
    const auto segments = split_tts_text(text);
    CHECK(segments == clauses);
    CHECK(concatenate(segments) == text);
}

HMI_TEST(tts_segments_keep_nearby_chinese_words_and_clause_tails_together)
{
    for (const std::string text : {
        "那嗯，你可以进行一下测试了。HMI已经安装了HGT CNN以及VAD。",
        "你好，当前程序已经升级到了新的大版本，可以开始使用。"})
    {
        const auto segments = split_tts_text(text);
        CHECK(concatenate(segments) == text);
        const std::string word = text.find("测试") != std::string::npos ? "测试" : "大版本";
        CHECK(std::any_of(segments.begin(), segments.end(), [&](const std::string &segment) {
            return segment.find(word) != std::string::npos;
        }));
        for (const auto &segment : segments) CHECK(utf8(segment));
    }
}

HMI_TEST(tts_segments_bound_long_unpunctuated_text_by_codepoints)
{
    std::string chinese;
    for (int index = 0; index < 170; ++index) chinese += "你";
    const auto segments = split_tts_text(chinese);
    CHECK(concatenate(segments) == chinese);
    CHECK(segments.size() == 5);
    CHECK(segments.front().size() == 12 * 3);
    CHECK(segments[1].size() == 24 * 3);
    CHECK(segments[2].size() == 48 * 3);
    const std::vector<std::size_t> budgets{12, 24, 48, 64, 64};
    for (std::size_t index = 0; index < segments.size(); ++index)
    {
        CHECK(utf8(segments[index]));
        CHECK(segments[index].size() <= budgets[index] * 3);
    }
    const std::string long_word(180, 'a');
    const auto words = split_tts_text(long_word);
    CHECK(concatenate(words) == long_word);
    CHECK(words.front().size() == 12);
    for (const auto &word : words) CHECK(word.size() <= 64);
}

HMI_TEST(tts_segments_ramp_up_after_a_brief_opening_to_avoid_a_long_second_request)
{
    std::string text = "你好，";
    for (int index = 0; index < 170; ++index) text += "你";
    const auto segments = split_tts_text(text);
    CHECK(concatenate(segments) == text);
    CHECK(segments.size() == 6);
    CHECK(segments.front() == "你好，");
    const std::vector<std::size_t> spoken_counts{2, 8, 16, 32, 64, 50};
    for (std::size_t index = 1; index < segments.size(); ++index)
    {
        CHECK(utf8(segments[index]));
        CHECK(segments[index].size() == spoken_counts[index] * 3);
        const auto budget = std::clamp(spoken_counts[index - 1] * 2, std::size_t{8}, std::size_t{64});
        CHECK(segments[index].size() <= budget * 3);
    }
}

HMI_TEST(tts_segments_do_not_treat_numeric_commas_or_time_colons_as_early_boundaries)
{
    const std::string amount = "金额12,345.67元今天到账。";
    const auto amounts = split_tts_text(amount);
    CHECK(concatenate(amounts) == amount);
    CHECK(amounts.front().find("12,345.67") != std::string::npos);
    const std::string time = "时间12:30开始播放语音。";
    const auto times = split_tts_text(time);
    CHECK(concatenate(times) == time);
    CHECK(times.front().find("12:30") != std::string::npos);
    for (const auto &segment : amounts) CHECK(utf8(segment));
    for (const auto &segment : times) CHECK(utf8(segment));
}

HMI_TEST(tts_segments_keep_decimal_values_and_attach_unspoken_separators)
{
    const std::string decimal = std::string(23, 'a') + "3.14元。再见。";
    const auto numbers = split_tts_text(decimal);
    CHECK(concatenate(numbers) == decimal);
    CHECK(std::any_of(numbers.begin(), numbers.end(), [](const std::string &part) {
        return part.find("3.14") != std::string::npos;
    }));
    const std::string first_boundary_decimal = std::string(11, 'a') + "3.14元。再见。";
    const auto first_numbers = split_tts_text(first_boundary_decimal);
    CHECK(concatenate(first_numbers) == first_boundary_decimal);
    CHECK(first_numbers.front() == std::string(11, 'a'));
    CHECK(first_numbers[1].find("3.14") != std::string::npos);
    const std::string text = "  。。。你好！！\n\n\t世界。   ";
    const auto segments = split_tts_text(text);
    CHECK(segments.size() == 2);
    CHECK(concatenate(segments) == text);
    CHECK(segments.front().find("你好") != std::string::npos);
    CHECK(segments.back().find("世界") != std::string::npos);
    CHECK(split_tts_text(" \t\r\n").empty());
    CHECK(split_tts_text("。。。！？").empty());
    CHECK(split_tts_text("\xe4\xbd").empty());
    CHECK(split_tts_text("\xc0\x80").empty());
    CHECK(split_tts_text(std::string("a\0b", 3)).empty());
}

HMI_TEST(tts_segments_keep_numbers_times_and_grouped_amounts_across_length_boundaries)
{
    const std::vector<std::string> tokens{
        "123456", "12:30", "12:30:45", "12,345.67", "1,234,567.89",
        "１２３４５６", "１２：３０", "１２，３４５．６７", "12：30", "12，345.67"};
    for (const auto &token : tokens)
    {
        // Exercise both the first request and later growing segment budgets.
        for (std::size_t prefix = 0; prefix < 80; ++prefix)
        {
            const auto text = std::string(prefix, 'a') + token + "元。朗读完成。";
            const auto segments = split_tts_text(text);
            CHECK(concatenate(segments) == text);
            CHECK(std::any_of(segments.begin(), segments.end(), [&](const std::string &part) {
                return part.find(token) != std::string::npos;
            }));
            for (const auto &segment : segments) CHECK(utf8(segment));
        }
    }
    const std::vector<std::pair<std::string, std::string>> examples{
        {"请确认今天的预约时间12:30再继续播报。", "12:30"},
        {"请确认今天的预约编号123456再继续播报。", "123456"},
        {"请确认今天的预约金额12,345.67再继续播报。", "12,345.67"}};
    for (const auto &[text, token] : examples)
    {
        const auto segments = split_tts_text(text);
        CHECK(concatenate(segments) == text);
        CHECK(segments.size() == 2);
        CHECK(segments.front().find(token) == std::string::npos);
        CHECK(segments[1].starts_with(token));
    }
    const std::string punctuation = "请确认数字123456。然后播放12：30，最后是12，345.67元。";
    const auto sentences = split_tts_text(punctuation);
    CHECK(concatenate(sentences) == punctuation);
    CHECK(std::any_of(sentences.begin(), sentences.end(), [](const std::string &part) {
        return part.find("123456。") != std::string::npos;
    }));
}

HMI_TEST(tts_segments_bound_exceptionally_long_numeric_identifiers)
{
    const std::string protected_number(64, '1');
    const auto intact = split_tts_text(protected_number + "号确认。");
    CHECK(intact.front() == protected_number);
    CHECK(concatenate(intact) == protected_number + "号确认。");
    const std::string identifier(180, '1');
    const auto split = split_tts_text(identifier);
    CHECK(concatenate(split) == identifier);
    CHECK(split.size() == 5);
    CHECK(split.front().size() == 12);
    for (const auto &segment : split) CHECK(segment.size() <= 64);
    const auto prefixed = split_tts_text(std::string(11, 'a') + identifier);
    CHECK(concatenate(prefixed) == std::string(11, 'a') + identifier);
    CHECK(prefixed.front().size() == 12);
    for (const auto &segment : prefixed) CHECK(segment.size() <= 64);
}

HMI_TEST(tts_segments_keep_short_english_words_and_recovery_values_whole)
{
    const std::vector<std::string> words{"telemetry", "Hello", "don't", "read-back", "log"};
    for (const auto &word : words)
    {
        for (std::size_t prefix = 0; prefix < 40; ++prefix)
        {
            std::string text;
            for (std::size_t index = 0; index < prefix; ++index) text += "前";
            text += word + "以及12,345.67元随后继续朗读。";
            const auto segments = split_tts_text(text);
            CHECK(concatenate(segments) == text);
            CHECK(std::any_of(segments.begin(), segments.end(), [&](const std::string &part) {
                return part.find(word) != std::string::npos;
            }));
            for (const auto &segment : segments) CHECK(utf8(segment));
        }
    }
    const std::string recovery = "请读 telemetry，数值12,345.67元，时间12:30。";
    const auto halves = split_tts_recovery(recovery);
    CHECK(halves.size() == 2 && concatenate(halves) == recovery);
    for (const auto &token : {"telemetry", "12,345.67", "12:30"})
        CHECK(std::any_of(halves.begin(), halves.end(), [&](const std::string &part) {
            return part.find(token) != std::string::npos;
        }));
    for (const auto &part : halves) CHECK(utf8(part));
    CHECK(split_tts_recovery("telemetry").empty());
    CHECK(split_tts_recovery("12,345.67").empty());
    CHECK(split_tts_recovery("字。 ").empty());
    CHECK(split_tts_recovery("\xe4\xbd").empty());
}

HMI_TEST(tts_pipeline_recovers_only_failed_segments_without_losing_or_repeating_text)
{
    const std::string text = "甲乙丙丁戊己庚辛壬癸。下一句。";
    std::vector<std::string> attempts;
    std::vector<std::string> generated;
    std::vector<std::size_t> heard;
    std::string error;
    TtsPipelineStatistics statistics;
    const bool ok = run_tts_pipeline(text,
        [&](std::string_view segment, hmi::audio::AudioBuffer &audio, std::string *detail) {
            attempts.emplace_back(segment);
            std::vector<tts_segmentation::Codepoint> points;
            if (!tts_segmentation::decode(segment, points)) return false;
            const auto spoken = std::count_if(points.begin(), points.end(), [](const auto &point) {
                return tts_segmentation::spoken(point.value);
            });
            if (spoken > 3)
            {
                if (detail) *detail = "typed length limit fixture";
                return false;
            }
            generated.emplace_back(segment);
            audio = pcm(static_cast<int>(generated.size()));
            return true;
        },
        [&](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
            return consume(read, detail, cancelled, [&](const hmi::audio::AudioBuffer &audio) {
                heard.push_back(static_cast<std::size_t>(audio.samples.front()) - 1);
            });
        }, {}, &error, &statistics, {}, [](std::string_view) { return SegmentFailureReason::LengthLimit; });
    CHECK(ok && error.empty());
    CHECK(statistics.finish_reason == "completed");
    CHECK(statistics.recovery_splits == 3 && statistics.recovery_attempts == 6);
    CHECK(!statistics.recovery_exhausted);
    CHECK(statistics.segments == 5 && statistics.generated_segments == 5 && heard.size() == 5);
    CHECK(statistics.queue_high_water <= 2);
    std::string spoken;
    for (const auto index : heard) spoken += generated.at(index);
    CHECK(spoken == text);
    CHECK(attempts.size() == split_tts_text(text).size() + statistics.recovery_attempts);
    CHECK(std::count(attempts.begin(), attempts.end(), attempts.front()) == 1);
}

HMI_TEST(tts_pipeline_does_not_recover_other_failures_or_skip_failed_segments)
{
    int attempts = 0;
    int classified = 0;
    std::string error;
    TtsPipelineStatistics statistics;
    CHECK(!run_tts_pipeline("甲乙。下一句。",
        [&](std::string_view, hmi::audio::AudioBuffer &, std::string *detail) {
            ++attempts;
            if (detail) *detail = "backend unavailable";
            return false;
        }, [](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
            return consume(read, detail, cancelled);
        }, {}, &error, &statistics, {}, [&](std::string_view) {
            ++classified;
            return SegmentFailureReason::Other;
        }));
    CHECK(error == "backend unavailable" && attempts == 1 && classified == 1);
    CHECK(statistics.finish_reason == "producer_failed");
    CHECK(statistics.recovery_splits == 0 && statistics.recovery_attempts == 0 && !statistics.recovery_exhausted);
}

HMI_TEST(tts_pipeline_bounds_recovery_depth_and_total_splits)
{
    for (const bool total_limit : {false, true})
    {
        std::string text;
        for (int index = 0; index < (total_limit ? 10 : 1); ++index)
            text += total_limit ? "甲乙。" : "甲乙丙丁戊己庚辛壬癸子丑。随后。";
        int attempts = 0;
        TtsPipelineStatistics statistics;
        std::string error;
        CHECK(!run_tts_pipeline(text,
            [&](std::string_view segment, hmi::audio::AudioBuffer &audio, std::string *detail) {
                ++attempts;
                if (total_limit && (segment == "甲" || segment == "乙。")) { audio = pcm(1); return true; }
                if (detail) *detail = "typed limit";
                return false;
            }, [](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
                return consume(read, detail, cancelled);
            }, {}, &error, &statistics, {}, [](std::string_view) { return SegmentFailureReason::LengthLimit; }));
        CHECK(error == "typed limit" && statistics.finish_reason == "generation_limit");
        CHECK(statistics.recovery_exhausted);
        CHECK(statistics.recovery_splits == (total_limit ? 8U : 3U));
        CHECK(statistics.recovery_attempts == (total_limit ? 16U : 3U));
        CHECK(attempts == (total_limit ? 25 : 4));
        CHECK(statistics.queue_high_water <= 2);
    }
}

HMI_TEST(tts_pipeline_does_not_retry_a_length_failure_after_cancellation)
{
    std::atomic<bool> cancelled{false};
    int attempts = 0;
    int classified = 0;
    TtsPipelineStatistics statistics;
    std::string error;
    CHECK(!run_tts_pipeline("甲乙丙丁。",
        [&](std::string_view, hmi::audio::AudioBuffer &, std::string *) {
            ++attempts;
            cancelled.store(true);
            return false;
        }, [](const ReadAudio &read, std::string *detail, const std::function<bool()> &stopped) {
            return consume(read, detail, stopped);
        }, [&] { return cancelled.load(); }, &error, &statistics, {}, [&](std::string_view) {
            ++classified;
            return SegmentFailureReason::LengthLimit;
        }));
    CHECK(attempts == 1 && classified == 0);
    CHECK(statistics.recovery_attempts == 0 && statistics.recovery_splits == 0);
    CHECK(statistics.finish_reason == "cancelled" && error == "speech interrupted");
}

HMI_TEST(tts_pipeline_stops_at_a_failed_replacement_and_never_retries_an_indivisible_word)
{
    for (const bool indivisible : {false, true})
    {
        std::vector<std::string> attempts;
        bool length_limited = false;
        TtsPipelineStatistics statistics;
        std::string error;
        CHECK(!run_tts_pipeline(indivisible ? "telemetry" : "甲乙。下一句。",
            [&](std::string_view segment, hmi::audio::AudioBuffer &audio, std::string *detail) {
                attempts.emplace_back(segment);
                length_limited = segment == "telemetry" || segment == "甲乙。";
                if (segment == "甲") { audio = pcm(1); return true; }
                if (detail) *detail = length_limited ? "typed limit" : "replacement failed";
                return false;
            }, [](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
                return consume(read, detail, cancelled);
            }, {}, &error, &statistics, {}, [&](std::string_view) {
                return length_limited ? SegmentFailureReason::LengthLimit : SegmentFailureReason::Other;
            }));
        if (indivisible)
        {
            CHECK((attempts == std::vector<std::string>{"telemetry"}));
            CHECK(statistics.recovery_exhausted && statistics.recovery_attempts == 0);
            CHECK(statistics.finish_reason == "generation_limit");
        }
        else
        {
            CHECK((attempts == std::vector<std::string>{"甲乙。", "甲", "乙。"}));
            CHECK(statistics.recovery_splits == 1 && statistics.recovery_attempts == 2);
            CHECK(statistics.finish_reason == "producer_failed" && error == "replacement failed");
        }
    }
}

HMI_TEST(tts_pipeline_plays_first_segment_before_the_rest_are_generated)
{
    std::atomic<int> generated{0};
    std::atomic<bool> first_playing{false};
    bool ordering = true;
    std::vector<int> heard;
    std::string error;
    TtsPipelineStatistics statistics;
    const bool ok = run_tts_pipeline("一。二。三。",
        [&](std::string_view, hmi::audio::AudioBuffer &audio, std::string *detail) {
            const int index = generated.fetch_add(1) + 1;
            if (index > 1 && !wait_until([&] { return first_playing.load(); }))
            {
                if (detail) *detail = "first segment was not consumed before continued synthesis";
                return false;
            }
            audio = pcm(index);
            return true;
        },
        [&](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
            return consume(read, detail, cancelled, [&](const hmi::audio::AudioBuffer &audio) {
                if (heard.empty())
                {
                    ordering = generated.load() < 3;
                    first_playing.store(true);
                }
                heard.push_back(static_cast<int>(audio.samples.front()));
            });
        }, {}, &error, &statistics);
    CHECK(ok);
    CHECK(error.empty());
    CHECK(ordering);
    CHECK((heard == std::vector<int>{1, 2, 3}));
    CHECK(statistics.segments == 3);
    CHECK(statistics.generated_segments == 3);
    CHECK(statistics.consumed_chunks == 3);
    CHECK(statistics.queue_high_water >= 1 && statistics.queue_high_water <= 2);
    CHECK(statistics.first_audio_ms >= 0.0 && statistics.first_audio_ms < statistics.total_ms);
}

HMI_TEST(tts_pipeline_applies_backpressure_before_generating_a_third_queued_segment)
{
    std::atomic<int> generated{0};
    bool held_at_two = false;
    std::string error;
    TtsPipelineStatistics statistics;
    const bool ok = run_tts_pipeline("一。二。三。四。",
        [&](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
            audio = pcm(generated.fetch_add(1) + 1);
            return true;
        },
        [&](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
            if (!wait_until([&] { return generated.load() >= 2; })) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            held_at_two = generated.load() == 2;
            return consume(read, detail, cancelled);
        }, {}, &error, &statistics);
    CHECK(ok);
    CHECK(held_at_two);
    CHECK(generated.load() == 4);
    CHECK(statistics.queue_high_water >= 1 && statistics.queue_high_water <= 2);
    CHECK(statistics.producer_wait_ms > 0.0);
}

HMI_TEST(tts_pipeline_observes_source_wait_without_counting_queued_audio_as_played)
{
    std::atomic<bool> source_pending{false};
    std::string error;
    TtsPipelineStatistics statistics;
    const bool ok = run_tts_pipeline("一。二。",
        [&](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
            if (!wait_until([&] { return source_pending.load(); })) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            audio = pcm(1);
            return true;
        },
        [&](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
            const ReadAudio observed = [&](hmi::audio::AudioBuffer &audio, std::string *failure) {
                const auto state = read(audio, failure);
                if (state == StreamRead::Pending) source_pending.store(true);
                return state;
            };
            return consume(observed, detail, cancelled);
        }, {}, &error, &statistics);
    CHECK(ok && error.empty());
    CHECK(statistics.consumer_pending_reads != 0);
    CHECK(statistics.consumer_wait_ms > 0.0);
    CHECK(statistics.consumed_chunks == 2 && statistics.generated_segments == 2);
}

HMI_TEST(tts_pipeline_does_not_count_pcm_cancelled_before_delivery)
{
    const auto player_thread = std::this_thread::get_id();
    std::atomic<bool> cancelled{false};
    hmi::audio::AudioBuffer delivered;
    bool rejected = false;
    TtsPipelineStatistics statistics;
    std::string error;
    CHECK(!run_tts_pipeline("你好。",
        [](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
            audio = pcm(1);
            return true;
        }, [&](const ReadAudio &read, std::string *detail, const std::function<bool()> &) {
            StreamRead result = StreamRead::Pending;
            const bool ready = wait_until([&] {
                result = read(delivered, detail);
                return result != StreamRead::Pending;
            });
            rejected = ready && result == StreamRead::Failed && delivered.samples.empty();
            return false;
        }, [&] {
            // Only the player owns this output buffer; the producer never reads it.
            if (std::this_thread::get_id() == player_thread && !delivered.samples.empty())
                cancelled.store(true);
            return cancelled.load();
        }, &error, &statistics));
    CHECK(rejected && cancelled.load());
    CHECK(statistics.generated_segments == 1 && statistics.consumed_chunks == 0);
    CHECK(statistics.finish_reason == "cancelled");
}

HMI_TEST(tts_pipeline_stops_consumer_wait_before_joining_cancelled_generation)
{
    std::atomic<bool> generation_started{false};
    std::atomic<bool> cancelled{false};
    bool pending_read = false;
    std::string error;
    TtsPipelineStatistics statistics;
    const bool ok = run_tts_pipeline("一。",
        [&](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
            generation_started.store(true);
            if (!wait_until([&] { return cancelled.load(); })) return false;
            // Simulate an SDK call that cannot return as soon as playback stops.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            audio = pcm(1);
            return true;
        },
        [&](const ReadAudio &read, std::string *, const std::function<bool()> &) {
            if (wait_until([&] { return generation_started.load(); }))
            {
                hmi::audio::AudioBuffer audio;
                pending_read = read(audio, nullptr) == StreamRead::Pending;
            }
            cancelled.store(true);
            return false;
        }, [&] { return cancelled.load(); }, &error, &statistics);
    CHECK(!ok && error == "speech interrupted");
    CHECK(pending_read && statistics.consumer_pending_reads == 1);
    CHECK(statistics.total_ms >= 80.0);
    CHECK(statistics.total_ms - statistics.consumer_wait_ms >= 80.0);
    CHECK(statistics.consumed_chunks == 0);
}

HMI_TEST(tts_pipeline_publishes_detached_progress_with_audio_frames_and_duration)
{
    std::vector<TtsPipelineStatistics> updates;
    int generated = 0;
    std::string error;
    TtsPipelineStatistics statistics;
    const bool ok = run_tts_pipeline("一。二。三。",
        [&](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
            ++generated;
            audio.channels = generated == 1 ? 2 : 1;
            audio.sample_rate = generated == 1 ? 24000 : generated == 2 ? 48000 : 8000;
            audio.samples.resize(static_cast<std::size_t>(audio.sample_rate / 10 * audio.channels), 0.0f);
            return true;
        },
        [](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
            return consume(read, detail, cancelled);
        }, {}, &error, &statistics,
        [&](const TtsPipelineStatistics &progress) { updates.push_back(progress); });
    CHECK(ok);
    CHECK(error.empty());
    CHECK(updates.size() == 5);
    CHECK(updates.front().segments == 3 && updates.front().generated_segments == 0);
    CHECK(updates.front().generated_audio_frames == 0 && updates.front().audio_seconds == 0.0);
    CHECK(statistics.generated_segments == 3 && statistics.generated_audio_frames == 8000);
    CHECK(std::fabs(statistics.audio_seconds - 0.3) < 0.000001);
    for (std::size_t index = 1; index < updates.size(); ++index)
    {
        CHECK(updates[index].generated_segments >= updates[index - 1].generated_segments);
        CHECK(updates[index].generated_audio_frames >= updates[index - 1].generated_audio_frames);
        CHECK(updates[index].audio_seconds >= updates[index - 1].audio_seconds);
        CHECK(updates[index].total_ms >= updates[index - 1].total_ms);
    }
    CHECK(updates[1].generated_segments == 1 && updates[1].generated_audio_frames == 2400);
    CHECK(updates[2].generated_segments == 2 && updates[2].generated_audio_frames == 7200);
    CHECK(updates.back().generated_audio_frames == statistics.generated_audio_frames);
    CHECK(updates.back().audio_seconds == statistics.audio_seconds);
}

HMI_TEST(tts_pipeline_preserves_published_progress_when_cancelled_before_playback)
{
    std::atomic<bool> cancelled{false};
    std::atomic<std::size_t> published{0};
    std::vector<TtsPipelineStatistics> updates;
    int generated = 0;
    bool observed_progress = false;
    std::string error;
    TtsPipelineStatistics statistics;
    const bool ok = run_tts_pipeline("一。二。三。",
        [&](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
            if (++generated > 1 && !wait_until([&] { return cancelled.load(); })) return false;
            audio.samples.resize(2400, 0.0f);
            return true;
        },
        [&](const ReadAudio &, std::string *, const std::function<bool()> &) {
            observed_progress = wait_until([&] { return published.load() == 1; });
            cancelled.store(true);
            return false;
        }, [&] { return cancelled.load(); }, &error, &statistics,
        [&](const TtsPipelineStatistics &progress) {
            updates.push_back(progress);
            published.store(progress.generated_segments);
        });
    CHECK(!ok);
    CHECK(observed_progress);
    CHECK(error == "speech interrupted");
    CHECK(statistics.segments == 3 && statistics.generated_segments == 1);
    CHECK(statistics.generated_audio_frames == 2400);
    CHECK(std::fabs(statistics.audio_seconds - 0.1) < 0.000001);
    CHECK(updates.size() == 3);
    CHECK(updates[1].generated_audio_frames == statistics.generated_audio_frames);
    CHECK(updates.back().generated_segments == 1);
    CHECK(updates.back().audio_seconds == statistics.audio_seconds);
}

HMI_TEST(tts_pipeline_cancels_with_empty_and_full_pcm_queues)
{
    for (const bool full : {false, true})
    {
        std::atomic<bool> cancelled{false};
        std::atomic<int> generated{0};
        bool reached_state = false;
        std::string error;
        TtsPipelineStatistics statistics;
        const auto started = Clock::now();
        const bool ok = run_tts_pipeline("一。二。三。四。",
            [&](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
                const int index = generated.fetch_add(1) + 1;
                if (!full && !wait_until([&] { return cancelled.load(); })) return false;
                audio = pcm(index);
                return true;
            },
            [&](const ReadAudio &, std::string *detail, const std::function<bool()> &) {
                reached_state = wait_until([&] { return generated.load() >= (full ? 2 : 1); });
                if (full) std::this_thread::sleep_for(std::chrono::milliseconds(25));
                cancelled.store(true);
                if (detail) *detail = "playback interrupted";
                return false;
            }, [&] { return cancelled.load(); }, &error, &statistics);
        CHECK(!ok);
        CHECK(reached_state);
        CHECK(error == "speech interrupted");
        CHECK(generated.load() == (full ? 2 : 1));
        CHECK(full ? statistics.queue_high_water >= 1 && statistics.queue_high_water <= 2 :
            statistics.queue_high_water == 0);
        CHECK(Clock::now() - started < std::chrono::seconds(2));
    }
}

HMI_TEST(tts_pipeline_reports_generation_failure_before_playback_interruption)
{
    std::atomic<int> generated{0};
    bool observed_cancel = false;
    std::string error;
    TtsPipelineStatistics statistics;
    const bool ok = run_tts_pipeline("一。二。三。",
        [&](std::string_view, hmi::audio::AudioBuffer &audio, std::string *detail) {
            const auto index = generated.fetch_add(1) + 1;
            if (index == 2)
            {
                if (detail) *detail = "second segment failed";
                return false;
            }
            audio = pcm(index);
            return true;
        },
        [&](const ReadAudio &, std::string *detail, const std::function<bool()> &cancelled) {
            observed_cancel = wait_until(cancelled);
            if (detail) *detail = "playback interrupted";
            return false;
        }, {}, &error, &statistics);
    CHECK(!ok);
    CHECK(observed_cancel);
    CHECK(generated.load() == 2);
    CHECK(error == "second segment failed");
    CHECK(statistics.finish_reason == "producer_failed");
}

HMI_TEST(tts_pipeline_propagates_playback_failure_and_joins_inflight_generation)
{
    std::atomic<bool> playback_failed{false};
    std::atomic<int> generated{0};
    bool first_seen = false;
    std::string error;
    const bool ok = run_tts_pipeline("一。二。三。",
        [&](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
            const int index = generated.fetch_add(1) + 1;
            if (index > 1 && !wait_until([&] { return playback_failed.load(); })) return false;
            audio = pcm(index);
            return true;
        },
        [&](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
            hmi::audio::AudioBuffer audio;
            first_seen = wait_until([&] {
                if (cancelled()) return true;
                return read(audio, detail) == StreamRead::Ready;
            }) && !audio.samples.empty();
            playback_failed.store(true);
            if (detail) *detail = "speaker unavailable";
            return false;
        }, {}, &error);
    CHECK(!ok);
    CHECK(first_seen);
    CHECK(error == "speaker unavailable");
    CHECK(generated.load() <= 2);
}

HMI_TEST(tts_pipeline_handles_callback_exceptions_and_rejects_invalid_pcm)
{
    std::string error;
    const StreamPlayer player = [](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
        return consume(read, detail, cancelled);
    };
    CHECK(!run_tts_pipeline("你好。", [](std::string_view, hmi::audio::AudioBuffer &, std::string *) -> bool {
        throw std::runtime_error("generation exception");
    }, player, {}, &error));
    CHECK(error.find("generation exception") != std::string::npos);
    CHECK(!run_tts_pipeline("你好。", [](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
        audio = pcm(1);
        return true;
    }, [](const ReadAudio &, std::string *, const std::function<bool()> &) -> bool {
        throw std::runtime_error("playback exception");
    }, {}, &error));
    CHECK(error.find("playback exception") != std::string::npos);
    CHECK(!run_tts_pipeline("你好。", [](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
        audio = pcm(1);
        audio.samples.front() = std::numeric_limits<float>::quiet_NaN();
        return true;
    }, player, {}, &error));
    CHECK(error == "TTS segment returned invalid PCM");
    CHECK(!run_tts_pipeline("你好。", [](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
        audio = pcm(1);
        return true;
    }, player, []() -> bool { throw std::runtime_error("cancel exception"); }, &error));
    CHECK(error.find("cancel exception") != std::string::npos);
    int progress_calls = 0;
    CHECK(run_tts_pipeline("你好。", [](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
        audio = pcm(1);
        return true;
    }, player, {}, &error, nullptr, [&](const TtsPipelineStatistics &) {
        ++progress_calls;
        throw std::runtime_error("progress exception");
    }));
    CHECK(error.empty());
    CHECK(progress_calls == 3);
}

HMI_TEST(tts_pipeline_does_not_report_success_before_playback_reads_completion)
{
    std::string error;
    CHECK(!run_tts_pipeline("你好。", [](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
        audio = pcm(1);
        return true;
    }, [](const ReadAudio &, std::string *, const std::function<bool()> &) { return true; }, {}, &error));
    CHECK(error.find("before all segments") != std::string::npos);

    std::atomic<bool> producer_finished{false};
    std::atomic<bool> final_audio_playing{false};
    std::atomic<bool> allow_finish{false};
    auto running = std::async(std::launch::async, [&] {
        return run_tts_pipeline("你好。", [&](std::string_view, hmi::audio::AudioBuffer &audio, std::string *) {
            audio = pcm(1);
            producer_finished.store(true);
            return true;
        }, [&](const ReadAudio &read, std::string *detail, const std::function<bool()> &cancelled) {
            return consume(read, detail, cancelled, [&](const hmi::audio::AudioBuffer &) {
                final_audio_playing.store(true);
                (void)wait_until([&] { return allow_finish.load(); });
            });
        }, {}, &error);
    });
    const bool reached_audio = wait_until([&] { return final_audio_playing.load(); });
    const bool still_waiting = running.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
    allow_finish.store(true);
    const bool ok = running.get();
    CHECK(reached_audio);
    CHECK(producer_finished.load());
    CHECK(still_waiting);
    CHECK(ok);
}
