#pragma once

#include "audio/audio_render.hpp"
#include "util/json_writer.hpp"

#include <cmath>

namespace hmi::audio::detail
{
// A pending snapshot must not assert that the device consumed zero frames.
inline util::json::Value playback_fields(const PlaybackStatistics &play)
{
    using util::json::Value;
    if (!play.finalized) return {};
    const auto number = [](double value) {
        return std::isfinite(value) && value >= 0.0 ? Value(value) : Value{};
    };
    const auto format = [](int value) { return value > 0 ? Value(static_cast<double>(value)) : Value{}; };
    return Value(Value::Object{
        {"endpoint", play.endpoint_id.empty() ? Value{} : Value(play.endpoint_id)},
        {"sample_rate", format(play.sample_rate)}, {"channels", format(play.channels)},
        {"mix_sample_rate", format(play.mix_sample_rate)}, {"mix_channels", format(play.mix_channels)},
        {"client_started", Value(play.client_started)},
        {"client_started_ms", play.client_started ? number(play.client_started_ms) : Value{}},
        {"submitted_frames", Value(static_cast<double>(play.submitted_frames))},
        {"consumed_frames", play.padding_available ? Value(static_cast<double>(play.consumed_frames)) : Value{}},
        {"padding_available", Value(play.padding_available)},
        {"accepted_chunks", Value(static_cast<double>(play.accepted_chunks))},
        {"completed_chunks", play.padding_available ? Value(static_cast<double>(play.completed_chunks)) : Value{}},
        {"underrun_events", Value(static_cast<double>(play.underrun_events))},
        {"pending_reads", Value(static_cast<double>(play.pending_reads))},
        {"source_wait_ms", number(play.source_wait_ms)}, {"wall_ms", number(play.wall_ms)},
        {"clipped_samples", Value(static_cast<double>(play.clipped_samples))},
        {"clock_available", Value(play.clock_available)},
        {"clock_frequency", play.clock_available ? Value(std::to_string(play.clock_frequency)) : Value{}},
        {"clock_start", play.clock_available ? Value(std::to_string(play.clock_start_position)) : Value{}},
        {"clock_end", play.clock_available ? Value(std::to_string(play.clock_end_position)) : Value{}},
        {"drained", Value(play.drained)}, {"finish_reason", Value(play.finish_reason)}});
}
}
