#pragma once

#include "base/capture_presentation.hpp"

namespace hmi::base
{
// Shared only by the desktop and application on the UI thread.
struct FrameActivity
{
    bool animation_active = false;
    CapturePresentation capture;
};

// Keep service polling independent of expensive GPU submission.
class FrameSchedule
{
public:
    double wait_seconds(double now) const noexcept
    {
        return next_tick_ > now ? next_tick_ - now : 0.0;
    }

    // Called once after each logical frame, including non-drawable frames.
    bool present_due(double now, bool drawable, bool animation_active,
                     bool interaction, bool surface_changed) noexcept
    {
        if (interaction)
            interactive_until_ = now + 0.25;
        const bool active = animation_active || now < interactive_until_;
        const bool active_started = active && !active_;
        const bool restored = drawable && !was_drawable_;
        active_ = active;
        was_drawable_ = drawable;
        if (!drawable)
            return false;

        const bool immediate = surface_changed || restored || active_started;
        if (!immediate && now + 1.0e-9 < next_present_)
            return false;

        const double interval = active_ ? 1.0 / 60.0 : 1.0 / 15.0;
        // Preserve cadence across 20 ms service ticks, but never catch up a
        // backlog after a slow frame, a suspended process or a hidden window.
        if (immediate || next_present_ + interval <= now)
            next_present_ = now + interval;
        else
            next_present_ += interval;
        return true;
    }

    void finish_tick(double started) noexcept
    {
        next_tick_ = started + (active_ && was_drawable_ ? 1.0 / 60.0 : 0.02);
    }

private:
    double next_tick_ = 0.0;
    double next_present_ = 0.0;
    double interactive_until_ = 0.0;
    bool was_drawable_ = false;
    bool active_ = false;
};
}
