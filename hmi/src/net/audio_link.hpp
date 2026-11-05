#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hmi::net
{
// Wire format of the ESP32 audio bridge (see esp32-175c/README.md):
//   12-byte header: A5 C3 | type | flags | seq(2) | len(2) | ts_ms(4)
enum class FrameType : std::uint8_t
{
    Hello = 1,
    AudioUp = 2,
    AudioDown = 3,
    Control = 4,
    Status = 5,
};

struct AudioLinkConfig
{
    std::string host = "172.16.30.145"; // Board address; it learns us from the first datagram.
    int port = 5000;
    int keepalive_ms = 2000; // The board forgets a peer that stays quiet for 10 s.
};

// One UDP socket, the bridge framing and the peer keepalive thread.
// Datagrams carry no ordering guarantee, so callers must tolerate loss.
class AudioLink
{
public:
    using FrameSink = std::function<void(FrameType type, const std::uint8_t *payload, std::size_t size)>;

    AudioLink();
    ~AudioLink();
    AudioLink(const AudioLink &) = delete;
    AudioLink &operator=(const AudioLink &) = delete;

    bool open(const AudioLinkConfig &config, std::string *error);
    void close();
    bool running() const noexcept;

    // Both are thread-safe; one datagram per call, no retransmission.
    bool send(FrameType type, const void *payload, std::size_t size);
    bool send_text(FrameType type, const std::string &text);

    // Starts the receive thread; the sink runs on it, so it must not block.
    bool start(FrameSink sink, std::string *error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
