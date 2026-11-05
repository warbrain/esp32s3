#pragma once

#include "audio/microphone_devices.hpp"
#include "camera/camera_devices.hpp"
#include "app/voice_runtime.hpp"
#include "app/settings.hpp"
#include "base/config.hpp"

#include <cstdint>
#include <vector>

namespace hmi::app
{
    // Owns every long-lived device object plus the per-frame samples derived from
    // them. The UI never enumerates devices itself; it reads this snapshot.
    class Services
    {
    public:
        Services();
        ~Services();

        void prepare();
        void start();
        void poll(float delta_seconds);
        void shutdown();
        VoiceRuntime &voice() noexcept { return voice_; }
        const VoiceRuntime &voice() const noexcept { return voice_; }
        VoiceSnapshot voice_snapshot() const;
        base::Logger *diagnostic_logger() noexcept { return &logger_; }
        bool set_tts_enabled(bool enabled, std::string *error = nullptr);

        void refresh_cameras();
        void refresh_microphones();

        audio::InputDevices &microphones() noexcept { return microphones_; }
        const audio::InputDevices &microphones() const noexcept { return microphones_; }

        const std::vector<camera::Device> &cameras() const noexcept { return cameras_; }

    private:
        void observe_microphone_selection();

        audio::InputDevices microphones_;
        std::vector<camera::Device> cameras_;
        base::Config configuration_;
        base::Logger logger_;
        Settings settings_;
        VoiceRuntime voice_;
        std::string configuration_file_;
        std::string configuration_error_;
        std::string logging_warning_;
        std::string tts_setting_error_;
        std::wstring observed_microphone_id_;
        bool microphone_selection_observed_ = false;
        std::uint64_t microphone_refreshes_ = 0;
        std::uint64_t camera_refreshes_ = 0;
        bool prepared_ = false;
        bool started_ = false;

        double log_flush_elapsed_seconds_ = 0.0;
    };
}
