#pragma once

#include "audio/audio_render.hpp"

#include <functional>
#include <string>

namespace hmi::audio::detail
{
enum class StreamRead
{
    Pending,
    Ready,
    Finished,
    Failed
};

// Ready transfers one owned PCM chunk; Pending must return without blocking.
using ReadAudio = std::function<StreamRead(AudioBuffer &, std::string *)>;

// Blocking on one playback worker. All chunks use one WASAPI client and format.
bool play_stream(const ReadAudio &read, std::string *error,
                 const std::function<bool()> &cancelled = {});
bool play_stream(const ReadAudio &read, std::string *error,
                 const std::function<bool()> &cancelled, PlaybackStatistics *statistics);
}
