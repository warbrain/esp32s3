#pragma once

#include "audiocpp.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace engine::capi {

// Keeps DSP and model implementations behind the SDK's opaque C handle.
class AudioFrontend {
public:
    explicit AudioFrontend(const audiocpp_audio_frontend_config & config);
    ~AudioFrontend();
    AudioFrontend(const AudioFrontend &) = delete;
    AudioFrontend & operator=(const AudioFrontend &) = delete;

    void reset(std::uint64_t request_id);
    std::vector<float> push(const float * samples, std::size_t frames);
    std::vector<float> finish();
    audiocpp_audio_frontend_statistics statistics() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace engine::capi
