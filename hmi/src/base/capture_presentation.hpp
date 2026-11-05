#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hmi::base
{
enum class CapturePhase
{
    Hidden,
    Preparing,
    Recording,
    Processing,
    AwaitingRelease
};

struct CapturePresentation
{
    CapturePhase phase = CapturePhase::Hidden;
    std::uint64_t request_id = 0;
    std::size_t captured_frames = 0;
    int limit_seconds = 30;
    std::array<float, 64> levels{};
    float live_level = 0.0f;
    std::array<char, 1024> transcript{}; // UTF-8 tail of the current capture, always NUL-terminated.
};
}
