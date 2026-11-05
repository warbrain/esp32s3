#include "check.hpp"
#include "base/frame_schedule.hpp"

#include <cmath>

HMI_TEST(idle_rendering_keeps_service_ticks_without_full_rate_gpu_submission)
{
    hmi::base::FrameSchedule schedule;
    int presents = 0;
    for (int tick = 0; tick < 50; ++tick)
    {
        const double now = static_cast<double>(tick) * 0.02;
        if (schedule.present_due(now, true, false, false, false))
            ++presents;
        schedule.finish_tick(now);
        CHECK(std::abs(schedule.wait_seconds(now) - 0.02) < 1.0e-9);
    }
    CHECK(presents == 15);
}

HMI_TEST(recording_and_interaction_keep_sixty_hertz_rendering)
{
    hmi::base::FrameSchedule recording;
    for (int tick = 0; tick < 60; ++tick)
    {
        const double now = static_cast<double>(tick) / 60.0;
        CHECK(recording.present_due(now, true, true, false, false));
        recording.finish_tick(now);
        CHECK(std::abs(recording.wait_seconds(now) - 1.0 / 60.0) < 1.0e-9);
    }

    hmi::base::FrameSchedule interaction;
    CHECK(interaction.present_due(0.0, true, false, false, false));
    CHECK(interaction.present_due(0.02, true, false, true, false));
    interaction.finish_tick(0.02);
    CHECK(std::abs(interaction.wait_seconds(0.02) - 1.0 / 60.0) < 1.0e-9);
    interaction.present_due(0.26, true, false, false, false);
    interaction.finish_tick(0.26);
    CHECK(std::abs(interaction.wait_seconds(0.26) - 1.0 / 60.0) < 1.0e-9);
    interaction.present_due(0.28, true, false, false, false);
    interaction.finish_tick(0.28);
    CHECK(std::abs(interaction.wait_seconds(0.28) - 0.02) < 1.0e-9);
}

HMI_TEST(hidden_window_keeps_polling_and_restores_without_waiting_for_idle_frame)
{
    hmi::base::FrameSchedule schedule;
    CHECK(schedule.present_due(0.0, true, false, false, false));
    for (int tick = 1; tick <= 50; ++tick)
    {
        const double now = static_cast<double>(tick) * 0.02;
        CHECK(!schedule.present_due(now, false, true, true, false));
        schedule.finish_tick(now);
        CHECK(std::abs(schedule.wait_seconds(now) - 0.02) < 1.0e-9);
    }
    CHECK(schedule.present_due(1.02, true, false, false, false));
}

HMI_TEST(resize_refreshes_immediately_and_a_stall_does_not_create_a_render_burst)
{
    hmi::base::FrameSchedule schedule;
    CHECK(schedule.present_due(0.0, true, false, false, false));
    CHECK(!schedule.present_due(0.02, true, false, false, false));
    CHECK(schedule.present_due(0.04, true, false, false, true));
    CHECK(!schedule.present_due(0.06, true, false, false, false));
    CHECK(schedule.present_due(30.0, true, false, false, false));
    CHECK(!schedule.present_due(30.02, true, false, false, false));

    schedule.finish_tick(30.0);
    CHECK(std::abs(schedule.wait_seconds(30.005) - 0.015) < 1.0e-9);
    CHECK(schedule.wait_seconds(30.02) < 1.0e-9);
    CHECK(schedule.wait_seconds(30.1) == 0.0);
}
