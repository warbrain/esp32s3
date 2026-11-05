#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hmi::capture
{
struct Format
{
    int sample_rate = 48000;
    int channels = 2;
    int bits_per_sample = 32;
    bool is_float = true;
};

// How the two capture channels become the mono stream the ASR consumes.
enum class Fusion
{
    Mean,  // average; this is what the OS downmix used to do
    Best,  // follow the channel with more energy (+1.5 dB hysteresis)
    Left,
    Right,
};



struct CaptureStatistics
{
    std::uint64_t open_attempts = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t timestamp_errors = 0;
    std::uint64_t silent_frames = 0;
    std::uint64_t dropped_partial_frames = 0;
    std::uint64_t delivered_frames = 0;
    std::uint64_t delivered_chunks = 0;
    std::string endpoint_id;
    std::string last_error;
};

// Borrowed PCM views remain valid only for the duration of the callback.
// interleaved contains the real negotiated channel count, never duplicated mono.
struct AudioBlock
{
    const float *mono = nullptr;
    const float *interleaved = nullptr;
    std::size_t frames = 0;
    int channels = 1;
    std::int64_t start_frame = 0;
};

// Captures from the default capture endpoint.
//
// The device keeps its native mix format (typically 48 kHz stereo float); the
// sink receives 16 kHz mono float because that is what Qwen3-ASR expects.
// Conversion is done by WASAPI (AUTOCONVERTPCM), not by us.
class AudioCapture
{
public:
    // Runs on the capture thread. samples holds `frames` mono float samples;
    // start_frame is the running sample counter at 16 kHz.
    using Sink = std::function<void(const float *samples, std::size_t frames, std::int64_t start_frame)>;
    using BlockSink = std::function<void(const AudioBlock &block)>;

    AudioCapture();
    ~AudioCapture();
    AudioCapture(const AudioCapture &) = delete;
    AudioCapture &operator=(const AudioCapture &) = delete;

    bool start(Sink sink, std::string *error);
    // Opens the endpoint with two channels when its mix format has at least
    // two, and fuses them here instead of accepting the OS downmix.
    bool start(Sink sink, std::string *error, Fusion fusion);
    // The richer callback keeps the original stereo samples alongside mono.
    bool start_blocks(BlockSink sink, std::string *error, Fusion fusion = Fusion::Mean);
    void stop();
    bool running() const noexcept;

    Format device_format() const noexcept; // native endpoint format
    Format stream_format() const noexcept; // what the sink receives: 16 kHz mono float
    Fusion fusion() const noexcept;
    int stream_channels() const noexcept; // 2 when the endpoint offered stereo

    float level() const noexcept; // 0..1 RMS of the last block
    // Detached counters survive stop(); no logging or allocation in the sink.
    CaptureStatistics statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
