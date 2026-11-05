#pragma once

#include <memory>
#include <cstdint>

namespace hmi::base { struct CapturePresentation; }

namespace hmi::platform
{
// Main UI thread only. Owns an input-transparent, nonactivating native overlay.
class CaptureOverlay
{
public:
    CaptureOverlay();
    ~CaptureOverlay();
    CaptureOverlay(const CaptureOverlay &) = delete;
    CaptureOverlay &operator=(const CaptureOverlay &) = delete;

    void update(const base::CapturePresentation &presentation);
    void hide() noexcept;
    bool visible() const noexcept;
    std::uint32_t last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
