#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hmi::audio
{
struct AudioBuffer
{
    std::vector<float> samples; // Interleaved, owned PCM.
    int sample_rate = 24000;
    int channels = 1;
};

struct PlaybackStatistics
{
    bool finalized = false; // False until the playback call has returned its final measurements.
    std::string endpoint_id;
    int sample_rate = 0;
    int channels = 0;
    int mix_sample_rate = 0;
    int mix_channels = 0;
    std::uint64_t submitted_frames = 0;
    std::uint64_t consumed_frames = 0; // Submitted minus device padding, not acoustic audibility.
    std::uint64_t accepted_chunks = 0;
    std::uint64_t completed_chunks = 0; // Chunk end has passed the observed device padding boundary.
    bool padding_available = false;
    std::uint64_t pending_reads = 0;
    std::uint64_t underrun_events = 0; // Source pending while device padding is empty.
    std::uint64_t clipped_samples = 0;
    double source_wait_ms = 0.0;
    double client_started_ms = 0.0; // Successful IAudioClient::Start, not the first audible sample.
    bool client_started = false;
    double wall_ms = 0.0;
    bool clock_available = false;
    std::uint64_t clock_frequency = 0;
    std::uint64_t clock_start_position = 0;
    std::uint64_t clock_end_position = 0;
    bool drained = false;
    std::string finish_reason;
};

class AudioRenderer
{
public:
    AudioRenderer();
    ~AudioRenderer();
    AudioRenderer(const AudioRenderer &) = delete;
    AudioRenderer &operator=(const AudioRenderer &) = delete;

    // Blocking; call only from a playback worker. stop() may run on any thread.
    bool play(const AudioBuffer &audio, std::string *error,
              const std::function<bool()> &cancelled = {});
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
