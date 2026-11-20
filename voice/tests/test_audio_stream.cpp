#include "check.hpp"
#include "audio/stream_playback.hpp"

#include <limits>
#include <stdexcept>

HMI_TEST(audio_stream_rejects_invalid_sources_before_opening_a_device)
{
    using hmi::audio::detail::StreamRead;
    std::string error;
    CHECK(!hmi::audio::detail::play_stream({}, &error));
    CHECK(!hmi::audio::detail::play_stream([](auto &, auto *) { return StreamRead::Finished; }, &error));
    CHECK(error.find("no PCM") != std::string::npos);
    CHECK(!hmi::audio::detail::play_stream([](auto &, auto *detail) {
        *detail = "source fixture failed";
        return StreamRead::Failed;
    }, &error));
    CHECK(error == "source fixture failed");
    CHECK(!hmi::audio::detail::play_stream([](auto &audio, auto *) {
        audio.samples = {std::numeric_limits<float>::quiet_NaN()};
        return StreamRead::Ready;
    }, &error));
    CHECK(error.find("non-finite") != std::string::npos);
}

HMI_TEST(audio_stream_waiting_can_be_cancelled_without_a_speaker)
{
    using hmi::audio::detail::StreamRead;
    int reads = 0;
    std::string error;
    CHECK(!hmi::audio::detail::play_stream([&](auto &, auto *) {
        ++reads;
        return StreamRead::Pending;
    }, &error, [&] { return reads >= 2; }));
    CHECK(reads == 2);
    CHECK(error.find("interrupted") != std::string::npos);
    CHECK(!hmi::audio::detail::play_stream([](auto &, auto *) -> StreamRead {
        throw std::runtime_error("source exception fixture");
    }, &error));
    CHECK(error.find("source exception fixture") != std::string::npos);
}

HMI_TEST(audio_stream_empty_source_reports_unavailable_device_measurements)
{
    using hmi::audio::detail::StreamRead;
    hmi::audio::PlaybackStatistics statistics;
    statistics.padding_available = statistics.client_started = statistics.clock_available = true;
    statistics.submitted_frames = statistics.consumed_frames = 42;
    std::string error;
    CHECK(!hmi::audio::detail::play_stream([](auto &, auto *) { return StreamRead::Finished; },
        &error, {}, &statistics));
    CHECK(error.find("no PCM") != std::string::npos);
    CHECK(statistics.finish_reason == "failed");
    CHECK(statistics.endpoint_id.empty());
    CHECK(!statistics.padding_available && !statistics.client_started && !statistics.clock_available);
    CHECK(statistics.submitted_frames == 0 && statistics.consumed_frames == 0);
    CHECK(statistics.accepted_chunks == 0 && statistics.completed_chunks == 0);
    CHECK(!statistics.drained && statistics.wall_ms >= 0.0);
    CHECK(statistics.finalized);
}

HMI_TEST(audio_stream_pending_cancellation_reports_wait_without_opening_a_device)
{
    using hmi::audio::detail::StreamRead;
    hmi::audio::PlaybackStatistics statistics;
    int reads = 0;
    std::string error;
    CHECK(!hmi::audio::detail::play_stream([&](auto &, auto *) {
        ++reads;
        return StreamRead::Pending;
    }, &error, [&] { return reads >= 2; }, &statistics));
    CHECK(reads == 2 && statistics.pending_reads == 2);
    CHECK(statistics.finish_reason == "interrupted");
    CHECK(statistics.source_wait_ms > 0.0 && statistics.wall_ms >= statistics.source_wait_ms);
    CHECK(statistics.endpoint_id.empty());
    CHECK(!statistics.padding_available && !statistics.client_started && !statistics.clock_available);
    CHECK(statistics.submitted_frames == 0 && statistics.consumed_frames == 0);
    CHECK(!statistics.drained);
    CHECK(statistics.finalized);
}

HMI_TEST(audio_stream_finalizes_rejections_and_exceptions_without_a_device)
{
    hmi::audio::PlaybackStatistics statistics;
    CHECK(!statistics.finalized);
    std::string error;
    CHECK(!hmi::audio::detail::play_stream({}, &error, {}, &statistics));
    CHECK(statistics.finalized && statistics.finish_reason == "failed");
    statistics = {};
    CHECK(!hmi::audio::detail::play_stream([](auto &, auto *) -> hmi::audio::detail::StreamRead {
        throw std::runtime_error("finalized fixture");
    }, &error, {}, &statistics));
    CHECK(statistics.finalized && statistics.finish_reason == "failed");
    CHECK(error.find("finalized fixture") != std::string::npos);
}
