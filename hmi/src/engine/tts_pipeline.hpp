#pragma once

#include "audio/stream_playback.hpp"
#include "engine/tts_segments.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace hmi::engine::detail
{
struct TtsPipelineStatistics
{
    std::size_t segments = 0;
    std::size_t generated_segments = 0; // Successfully published PCM segments.
    std::size_t generated_audio_frames = 0; // Per-channel PCM frames, not played frames.
    std::size_t queue_high_water = 0;
    double first_audio_ms = 0.0; // PCM ready, not the first audible sample.
    double synthesis_ms = 0.0;
    double total_ms = 0.0;
    double producer_wait_ms = 0.0;
    double consumer_wait_ms = 0.0;
    std::size_t consumer_pending_reads = 0;
    std::size_t consumed_chunks = 0; // Delivered to the player, not necessarily drained by the device.
    double audio_seconds = 0.0;
    std::size_t recovery_splits = 0;
    std::size_t recovery_attempts = 0; // Synthesis calls for replacement segments, excluding original segments.
    bool recovery_exhausted = false;
    std::string finish_reason;
};

using SegmentSynthesizer = std::function<bool(std::string_view, audio::AudioBuffer &, std::string *)>;
using StreamPlayer = std::function<bool(const audio::detail::ReadAudio &, std::string *,
                                       const std::function<bool()> &)>;
// Short observer of detached snapshots; calls never overlap. Exceptions are ignored.
using TtsPipelineProgress = std::function<void(const TtsPipelineStatistics &)>;
enum class SegmentFailureReason { Other, LengthLimit };
// Called only after synthesis fails and cancellation has been checked. SDK
// adapters classify their own typed outcome; the pipeline never parses errors.
using SegmentRecovery = std::function<SegmentFailureReason(std::string_view)>;

inline bool run_tts_pipeline(std::string_view text, const SegmentSynthesizer &synthesize,
                             const StreamPlayer &play, const std::function<bool()> &cancelled,
                             std::string *error, TtsPipelineStatistics *statistics = nullptr,
                             const TtsPipelineProgress &progress = {}, const SegmentRecovery &recovery = {})
{
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto elapsed = [](Clock::time_point since) {
        return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    };
    if (error) error->clear();
    if (statistics) *statistics = {};
    const auto segments = split_tts_text(text);
    if (segments.empty() || !synthesize || !play)
    {
        if (error) *error = "TTS requires valid spoken text and generation/playback callbacks";
        if (statistics) statistics->finish_reason = "producer_failed";
        return false;
    }
    struct Pipeline
    {
        std::mutex mutex;
        std::condition_variable available;
        std::deque<audio::AudioBuffer> pcm;
        std::atomic<bool> stop{false};
        std::atomic<bool> failed{false};
        bool complete = false;
        bool finished_read = false;
        std::string failure;
        std::string failure_reason;
        double consumer_wait_ms = 0.0;
        std::size_t consumer_pending_reads = 0;
        std::size_t consumed_chunks = 0;
    } pipeline;
    TtsPipelineStatistics measured;
    measured.segments = segments.size();
    const auto publish_progress = [&]() noexcept {
        if (!progress) return;
        auto snapshot = measured;
        {
            std::lock_guard lock(pipeline.mutex);
            snapshot.consumer_wait_ms = pipeline.consumer_wait_ms;
            snapshot.consumer_pending_reads = pipeline.consumer_pending_reads;
            snapshot.consumed_chunks = pipeline.consumed_chunks;
        }
        snapshot.total_ms = elapsed(started);
        try { progress(snapshot); }
        catch (...) { }
    };
    publish_progress();
    const auto fail = [&pipeline](std::string detail, std::string reason = "producer_failed") {
        {
            std::lock_guard lock(pipeline.mutex);
            if (pipeline.failure.empty())
            {
                pipeline.failure = std::move(detail);
                pipeline.failure_reason = std::move(reason);
            }
            pipeline.pcm.clear();
            pipeline.failed.store(true);
        }
        pipeline.available.notify_all();
    };
    const auto external_cancelled = [&] {
        if (!cancelled) return false;
        try { return cancelled(); }
        catch (const std::exception &exception) { fail(std::string("TTS cancellation failed: ") + exception.what()); }
        catch (...) { fail("TTS cancellation failed"); }
        return true;
    };
    const std::function<bool()> interrupted = [&] {
        return pipeline.stop.load() || pipeline.failed.load() || external_cancelled();
    };
    Clock::time_point consumer_wait_started{};
    const audio::detail::ReadAudio read = [&](audio::AudioBuffer &out, std::string *detail) {
        out = {};
        const bool stopped = interrupted();
        audio::detail::StreamRead result = audio::detail::StreamRead::Pending;
        {
            std::lock_guard lock(pipeline.mutex);
            if (!pipeline.failure.empty())
            {
                if (detail) *detail = pipeline.failure;
                return audio::detail::StreamRead::Failed;
            }
            if (stopped)
            {
                if (detail) *detail = "speech interrupted";
                return audio::detail::StreamRead::Failed;
            }
            if (!pipeline.pcm.empty())
            {
                out = std::move(pipeline.pcm.front());
                pipeline.pcm.pop_front();
                result = audio::detail::StreamRead::Ready;
            }
            else if (pipeline.complete)
            {
                pipeline.finished_read = true;
                result = audio::detail::StreamRead::Finished;
            }
            if (result == audio::detail::StreamRead::Pending)
            {
                ++pipeline.consumer_pending_reads;
                if (consumer_wait_started == Clock::time_point{}) consumer_wait_started = Clock::now();
            }
            else if (consumer_wait_started != Clock::time_point{})
            {
                pipeline.consumer_wait_ms += elapsed(consumer_wait_started);
                consumer_wait_started = {};
            }
        }
        pipeline.available.notify_one();
        if (result == audio::detail::StreamRead::Ready)
        {
            if (interrupted())
            {
                out = {};
                if (detail) *detail = "speech interrupted";
                return audio::detail::StreamRead::Failed;
            }
            std::lock_guard lock(pipeline.mutex);
            ++pipeline.consumed_chunks;
        }
        return result;
    };
    std::thread producer;
    try
    {
        producer = std::thread([&] {
            struct Complete
            {
                Pipeline &pipeline;
                ~Complete()
                {
                    { std::lock_guard lock(pipeline.mutex); pipeline.complete = true; }
                    pipeline.available.notify_all();
                }
            } complete{pipeline};
            try
            {
                struct Segment { std::string text; std::size_t depth = 0; };
                std::deque<Segment> pending;
                for (const auto &segment : segments) pending.push_back({segment, 0});
                while (!pending.empty())
                {
                    // Wait before generating, so slow playback cannot trigger
                    // unbounded prefetch or an unnecessary third queued segment.
                    for (;;)
                    {
                        if (interrupted()) return;
                        std::unique_lock lock(pipeline.mutex);
                        if (pipeline.pcm.size() < 2) break;
                        const auto waiting = Clock::now();
                        pipeline.available.wait_for(lock, std::chrono::milliseconds(20));
                        measured.producer_wait_ms += elapsed(waiting);
                    }
                    if (interrupted()) return;
                    auto segment = std::move(pending.front());
                    pending.pop_front();
                    audio::AudioBuffer audio;
                    std::string detail;
                    if (segment.depth != 0) ++measured.recovery_attempts;
                    const auto generation_started = Clock::now();
                    const bool ok = synthesize(segment.text, audio, &detail);
                    measured.synthesis_ms += elapsed(generation_started);
                    if (interrupted()) return;
                    if (!ok)
                    {
                        const auto reason = recovery ? recovery(segment.text) : SegmentFailureReason::Other;
                        if (interrupted()) return;
                        if (reason == SegmentFailureReason::LengthLimit)
                        {
                            const auto replacements = segment.depth < 3 && measured.recovery_splits < 8 ?
                                split_tts_recovery(segment.text) : std::vector<std::string>{};
                            if (replacements.size() == 2 && replacements[0] + replacements[1] == segment.text)
                            {
                                ++measured.recovery_splits;
                                ++measured.segments;
                                pending.push_front({replacements[1], segment.depth + 1});
                                pending.push_front({replacements[0], segment.depth + 1});
                                publish_progress();
                                continue;
                            }
                            measured.recovery_exhausted = true;
                        }
                        fail(detail.empty() ? "TTS segment generation failed" : std::move(detail),
                            reason == SegmentFailureReason::LengthLimit ? "generation_limit" : "producer_failed");
                        return;
                    }
                    if (interrupted()) return;
                    if (audio.sample_rate < 8000 || audio.sample_rate > 192000 || audio.channels < 1 ||
                        audio.channels > 8 || audio.samples.empty() ||
                        audio.samples.size() % static_cast<std::size_t>(audio.channels) != 0 ||
                        audio.samples.size() / static_cast<std::size_t>(audio.channels) >
                            static_cast<std::size_t>(audio.sample_rate) * 300 ||
                        !std::all_of(audio.samples.begin(), audio.samples.end(), [](float value) { return std::isfinite(value); }))
                    {
                        fail("TTS segment returned invalid PCM");
                        return;
                    }
                    const auto audio_frames = audio.samples.size() / static_cast<std::size_t>(audio.channels);
                    const auto audio_seconds = static_cast<double>(audio_frames) / audio.sample_rate;
                    {
                        std::lock_guard lock(pipeline.mutex);
                        if (pipeline.stop.load() || pipeline.failed.load()) return;
                        pipeline.pcm.push_back(std::move(audio));
                        ++measured.generated_segments;
                        measured.generated_audio_frames += audio_frames;
                        measured.audio_seconds += audio_seconds;
                        if (measured.generated_segments == 1) measured.first_audio_ms = elapsed(started);
                        measured.queue_high_water = std::max(measured.queue_high_water, pipeline.pcm.size());
                    }
                    pipeline.available.notify_all();
                    publish_progress();
                }
            }
            catch (const std::exception &exception) { fail(std::string("TTS generation failed: ") + exception.what()); }
            catch (...) { fail("TTS generation failed unexpectedly"); }
        });
    }
    catch (const std::exception &exception)
    {
        if (error) *error = std::string("Cannot start TTS generation: ") + exception.what();
        if (statistics) statistics->finish_reason = "producer_failed";
        return false;
    }
    struct JoinProducer
    {
        Pipeline &pipeline;
        std::thread &thread;
        ~JoinProducer()
        {
            pipeline.stop.store(true);
            pipeline.available.notify_all();
            if (thread.joinable()) thread.join();
        }
    } join{pipeline, producer};
    bool played = false;
    std::string playback_error;
    try { played = play(read, &playback_error, interrupted); }
    catch (const std::exception &exception) { playback_error = std::string("TTS playback failed: ") + exception.what(); }
    catch (...) { playback_error = "TTS playback failed unexpectedly"; }
    {
        std::lock_guard lock(pipeline.mutex);
        if (consumer_wait_started != Clock::time_point{})
        {
            pipeline.consumer_wait_ms += elapsed(consumer_wait_started);
            consumer_wait_started = {};
        }
    }
    const bool was_cancelled = external_cancelled();
    pipeline.stop.store(true);
    pipeline.available.notify_all();
    producer.join();
    measured.total_ms = elapsed(started);
    {
        std::lock_guard lock(pipeline.mutex);
        measured.consumer_wait_ms = pipeline.consumer_wait_ms;
        measured.consumer_pending_reads = pipeline.consumer_pending_reads;
        measured.consumed_chunks = pipeline.consumed_chunks;
    }
    bool completed = false;
    {
        std::lock_guard lock(pipeline.mutex);
        if (!pipeline.failure.empty())
        {
            if (error) *error = pipeline.failure;
            measured.finish_reason = pipeline.failure_reason;
        }
        else if (!played || was_cancelled || !pipeline.finished_read)
        {
            if (error) *error = was_cancelled ? "speech interrupted" : !playback_error.empty() ?
                playback_error : "speech playback ended before all segments completed";
            measured.finish_reason = was_cancelled ? "cancelled" : "playback_failed";
        }
        else
        {
            measured.finish_reason = "completed";
            completed = true;
        }
    }
    if (statistics) *statistics = measured;
    publish_progress();
    return completed;
}
}
