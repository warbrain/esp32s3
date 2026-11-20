#pragma once

#include "net/audio_link.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hmi::net
{
// The ESP32-S3 board as the microphone and speaker of the HMI.
//
// Uplink is Opus stereo at 16 kHz (both microphones); the sink receives 16 kHz
// mono float, which is the same format capture::AudioCapture hands to ASR.
class Esp32Audio
{
public:
    using MicSink = std::function<void(const float *mono, std::size_t frames, std::int64_t start_frame)>;
    // Board-side button events arrive as Control frames ("ptt_down" / "ptt_up").
    using ControlSink = std::function<void(const char *text, std::size_t size)>;

    Esp32Audio();
    ~Esp32Audio();
    Esp32Audio(const Esp32Audio &) = delete;
    Esp32Audio &operator=(const Esp32Audio &) = delete;

    // Connects, asks the board to start streaming and decodes until stop().
    bool start(const AudioLinkConfig &config, MicSink sink, ControlSink control, std::string *error);
    void stop();
    bool running() const noexcept;

    // Queues 16 kHz mono float for the board's speaker and returns immediately;
    // a dedicated player thread encodes and paces 20 ms Opus frames in real time.
    // Use cancel_playback() to drop queued audio when the reply is cancelled.
    bool play(const float *samples, std::size_t frames, int sample_rate = 16000);

    // Drops every queued buffer and stops the item being sent.
    void cancel_playback();

    // Blocks until the player thread has drained the queue (shutdown/tests).
    void flush_playback();

    // Buffers still waiting for the player thread.
    std::size_t playback_backlog() const;

    // Starts or stops the board's uplink without closing the link; TTS playback
    // must keep working after the microphone is released.
    bool request_capture(bool on);

    // Board speaker volume, 0..100; the board persists it in NVS.
    bool set_volume(int percent);

    // Diagnostics from the last stream.
    struct Statistics
    {
        std::uint64_t frames_received = 0;
        std::uint64_t decode_errors = 0;
        std::uint64_t frames_sent = 0;
        std::uint64_t send_errors = 0;
        std::string last_error;
    };
    Statistics statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
