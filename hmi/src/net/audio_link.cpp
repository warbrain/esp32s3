#include "net/audio_link.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace hmi::net
{
namespace
{
constexpr std::uint8_t kMagic0 = 0xA5;
constexpr std::uint8_t kMagic1 = 0xC3;
constexpr std::size_t kHeaderBytes = 12;

void put_u16(std::uint8_t *p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void put_u32(std::uint8_t *p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

std::uint16_t read_u16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

bool winsock_ready()
{
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}
} // namespace

struct AudioLink::Impl
{
    AudioLinkConfig config;
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in target{};
    std::atomic<bool> running{false};
    std::thread worker;
    FrameSink sink;
    std::mutex send_mutex;
    std::uint16_t sequence = 0;
};

AudioLink::AudioLink() : impl_(std::make_unique<Impl>()) {}

AudioLink::~AudioLink()
{
    close();
}

bool AudioLink::open(const AudioLinkConfig &config, std::string *error)
{
    if (!winsock_ready())
    {
        if (error) *error = "WSAStartup failed";
        return false;
    }
    if (impl_->socket != INVALID_SOCKET)
    {
        return true;
    }

    impl_->config = config;
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == INVALID_SOCKET)
    {
        if (error) *error = "socket() failed";
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (bind(impl_->socket, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0)
    {
        if (error) *error = "bind() failed";
        closesocket(impl_->socket);
        impl_->socket = INVALID_SOCKET;
        return false;
    }

    int buffer_bytes = 1 << 20;
    setsockopt(impl_->socket, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char *>(&buffer_bytes), sizeof(buffer_bytes));

    // Wake up regularly so the keepalive can run even when the board is silent.
    DWORD timeout_ms = 200;
    setsockopt(impl_->socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));

    impl_->target = {};
    impl_->target.sin_family = AF_INET;
    impl_->target.sin_port = htons(static_cast<unsigned short>(config.port));
    if (InetPtonA(AF_INET, config.host.c_str(), &impl_->target.sin_addr) != 1)
    {
        if (error) *error = "invalid board address: " + config.host;
        close();
        return false;
    }
    return true;
}

bool AudioLink::start(FrameSink sink, std::string *error)
{
    if (impl_->socket == INVALID_SOCKET)
    {
        if (error) *error = "audio link is not open";
        return false;
    }
    if (impl_->running.load())
    {
        return true;
    }

    impl_->sink = std::move(sink);
    impl_->running.store(true);
    impl_->worker = std::thread([this] {
        std::vector<std::uint8_t> packet(2048);
        auto last_keepalive = std::chrono::steady_clock::now();
        while (impl_->running.load())
        {
            sockaddr_in from{};
            int from_size = sizeof(from);
            const int received = recvfrom(impl_->socket, reinterpret_cast<char *>(packet.data()),
                                          static_cast<int>(packet.size()), 0,
                                          reinterpret_cast<sockaddr *>(&from), &from_size);
            if (received < static_cast<int>(kHeaderBytes))
            {
                // Idle: keep the board's peer entry alive so it does not forget us.
                const auto now = std::chrono::steady_clock::now();
                const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - last_keepalive)
                                         .count();
                if (idle_ms >= impl_->config.keepalive_ms)
                {
                    send_text(FrameType::Control, "ping");
                    last_keepalive = now;
                }
                continue;
            }

            if (packet[0] != kMagic0 || packet[1] != kMagic1)
            {
                continue;
            }
            const auto type = static_cast<FrameType>(packet[2]);
            const std::uint16_t length = read_u16(packet.data() + 6);
            if (static_cast<std::size_t>(received) < kHeaderBytes + length)
            {
                continue;
            }
            if (impl_->sink)
            {
                impl_->sink(type, packet.data() + kHeaderBytes, length);
            }
        }
    });
    return true;
}

void AudioLink::close()
{
    impl_->running.store(false);
    if (impl_->worker.joinable())
    {
        impl_->worker.join();
    }
    if (impl_->socket != INVALID_SOCKET)
    {
        closesocket(impl_->socket);
        impl_->socket = INVALID_SOCKET;
    }
}

bool AudioLink::running() const noexcept
{
    return impl_->running.load();
}

bool AudioLink::send(FrameType type, const void *payload, std::size_t size)
{
    if (impl_->socket == INVALID_SOCKET || size + kHeaderBytes > 1600)
    {
        return false;
    }

    std::uint8_t packet[1600];
    packet[0] = kMagic0;
    packet[1] = kMagic1;
    packet[2] = static_cast<std::uint8_t>(type);
    packet[3] = 0;

    std::lock_guard lock(impl_->send_mutex);
    put_u16(packet + 4, impl_->sequence++);
    put_u16(packet + 6, static_cast<std::uint16_t>(size));
    put_u32(packet + 8, static_cast<std::uint32_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count()));
    if (size > 0 && payload != nullptr)
    {
        std::memcpy(packet + kHeaderBytes, payload, size);
    }

    const int sent = sendto(impl_->socket, reinterpret_cast<const char *>(packet),
                            static_cast<int>(kHeaderBytes + size), 0,
                            reinterpret_cast<const sockaddr *>(&impl_->target),
                            sizeof(impl_->target));
    return sent == static_cast<int>(kHeaderBytes + size);
}

bool AudioLink::send_text(FrameType type, const std::string &text)
{
    return send(type, text.data(), text.size());
}
}
