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

struct AudioPreprocessorConfig
{
    bool vad_enabled = true;
    bool denoise_enabled = false;
    std::filesystem::path vad_model;
    std::filesystem::path denoise_model;
    float vad_threshold = 0.35f;
    int maximum_audio_seconds = 30;
};

struct AudioPreprocessorStatistics
{
    bool ready = false;
    bool vad_enabled = false;
    bool denoise_enabled = false;
    bool failed = false;
    bool finished = false;
    bool raw_fallback = false;
    std::uint64_t request_id = 0;
    std::uint64_t input_frames = 0;
    std::uint64_t output_frames = 0;
    std::uint64_t gtcrn_frames = 0;
    std::uint64_t vad_chunks = 0;
    std::uint64_t voiced_chunks = 0;
    std::uint64_t uncertain_chunks = 0;
    std::uint64_t voiced_frames = 0;
    std::uint64_t pending_frames = 0;
    float latest_vad_probability = 0.0f;
    float max_vad_probability = 0.0f;
    bool speech_seen = false;
    bool speech_active = false;
    double gtcrn_ms = 0.0;
    double vad_ms = 0.0;
    double wall_ms = 0.0;
};

// Owns one SDK frontend and its streaming state. Use only on the ASR worker.
// Returned PCM keeps the complete input timeline, including final lookahead.
class AudioPreprocessor
{
public:
    AudioPreprocessor();
    ~AudioPreprocessor();
    AudioPreprocessor(const AudioPreprocessor &) = delete;
    AudioPreprocessor &operator=(const AudioPreprocessor &) = delete;

    bool open(Client &client, const AudioPreprocessorConfig &config, std::string *error);
    void close();
    bool ready() const noexcept;
    bool begin(std::uint64_t request_id, std::string *error);
    bool push(const float *samples, std::size_t frames, std::vector<float> &out, std::string *error);
    bool finish(std::vector<float> &out, std::string *error);
    AudioPreprocessorStatistics statistics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
