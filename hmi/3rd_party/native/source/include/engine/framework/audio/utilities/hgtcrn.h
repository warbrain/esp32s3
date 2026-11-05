#pragma once

#include "engine/framework/core/backend.h"

#include <cstddef>
#include <cstdint>
#include <complex>
#include <filesystem>
#include <memory>
#include <vector>

namespace engine::audio {

// Internal diagnostic view only. Data is borrowed for the synchronous callback,
// laid out [channels, frequency, frames] as interleaved complex float values.
struct HgtcrnStageObserver {
    using Callback = void (*)(void * context, const char * stage, const std::complex<float> * data,
                              std::size_t channels, std::size_t frequency, std::size_t frames);
    void * context = nullptr;
    Callback callback = nullptr;
};

struct HgtcrnStatistics {
    std::uint64_t request_id = 0;
    std::uint64_t input_frames = 0;
    std::uint64_t output_frames = 0;
    std::uint64_t stft_frames = 0;
    std::uint32_t wpe_taps = 18;
    std::uint32_t iva_iterations = 0;
    std::uint32_t iva_speech_channel = 0;
    std::uint32_t right_padding_frames = 0;
    bool succeeded = false;
    bool failed = false;
    double channel_correlation = 0.0;
    double channel_rms_ratio = 0.0;
    double stft_ms = 0.0;
    double wpe_ms = 0.0;
    double iva_ms = 0.0;
    double network_ms = 0.0;
    double istft_ms = 0.0;
    double total_ms = 0.0;
};

// Full-segment masking-on-noisy H-GTCRN. Requires real synchronous dual-channel
// 16 kHz PCM, never duplicated mono. WPE/AuxIVA are deliberately not streaming.
class HgtcrnModel {
public:
    HgtcrnModel(const std::filesystem::path & checkpoint, const core::BackendConfig & backend,
                std::size_t max_frames = 480000);
    ~HgtcrnModel();
    HgtcrnModel(const HgtcrnModel &) = delete;
    HgtcrnModel & operator=(const HgtcrnModel &) = delete;

    std::vector<float> run(const float * interleaved, std::size_t frames, std::uint64_t request_id,
                           const HgtcrnStageObserver * observer = nullptr);
    HgtcrnStatistics statistics() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace engine::audio
