#pragma once

#include "engine/native_client.hpp"
#include "engine/model_location.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hmi::engine
{
struct Transcript
{
    // push() emits cumulative replacement snapshots; finish() emits the final text.
    std::string text;
    bool final = true;
    float confidence = -1.0f; // Negative means the model did not report it.
};

struct AsrConfig
{
    std::string model_path = default_model_path("asr.gguf");
    std::string backend = "auto";
    std::string language = "zh";
    int threads = 4;
    double preview_interval_seconds = 1.0;
    bool refine = true;
    int maximum_audio_seconds = 30;
};

struct AsrStatistics
{
    std::string backend;
    int threads = 0;
    std::size_t audio_frames = 0;
    std::size_t preview_count = 0;
    double preview_ms = 0.0;
    double refine_ms = 0.0;
};

// Single-threaded engine wrapper: all methods belong to the ASR worker.
// The Client must remain open until this wrapper is closed.
class AsrSession
{
public:
    AsrSession();
    ~AsrSession();
    AsrSession(const AsrSession &) = delete;
    AsrSession &operator=(const AsrSession &) = delete;

    bool open(Client &client, const AsrConfig &config, std::string *error);
    void close();
    bool ready() const noexcept;
    bool begin(std::string *error, std::uint64_t request_id = 0);
    // Disable previews while draining an already-ended capture; all PCM is kept
    // for finish(). The flag never changes the final refinement policy.
    bool push(const float *samples, std::size_t frames, std::vector<Transcript> &out,
              std::string *error, bool allow_preview = true);
    bool finish(Transcript &out, std::string *error);
    void reset();
    // Host feed quantum, at most 250 ms; SDK transport keeps its own chunk size.
    std::size_t preferred_chunk_samples() const noexcept;
    // Called only by the session's owning SDK thread.
    AsrStatistics statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
