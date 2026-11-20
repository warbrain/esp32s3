#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hmi::engine
{
class Client;

struct HgtcrnConfig
{
    std::filesystem::path model;
    int maximum_audio_seconds = 30;
};

struct HgtcrnStatistics
{
    bool ready = false;
    bool succeeded = false;
    bool failed = false;
    std::uint32_t channels = 0;
    std::uint64_t request_id = 0;
    std::uint64_t input_frames = 0;
    std::uint64_t output_frames = 0;
    std::uint64_t stft_frames = 0;
    std::uint32_t wpe_taps = 0;
    std::uint32_t iva_iterations = 0;
    std::uint32_t iva_speech_channel = 0;
    std::uint32_t right_padding_frames = 0;
    double channel_correlation = 0.0;
    double channel_rms_ratio = 0.0;
    double stft_ms = 0.0;
    double wpe_ms = 0.0;
    double iva_ms = 0.0;
    double network_ms = 0.0;
    double istft_ms = 0.0;
    double total_ms = 0.0;
};

// Full-segment, real dual-channel enhancement through the native SDK only.
// One call starts fresh state; a failed request can be followed by another run.
class HgtcrnSession
{
public:
    HgtcrnSession();
    ~HgtcrnSession();
    HgtcrnSession(const HgtcrnSession &) = delete;
    HgtcrnSession &operator=(const HgtcrnSession &) = delete;

    bool open(Client &client, const HgtcrnConfig &config, std::string *error);
    void close();
    bool ready() const noexcept;
    bool run(const float *interleaved, std::size_t frames, std::uint64_t request_id,
             std::vector<float> &mono, std::string *error);
    HgtcrnStatistics statistics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
