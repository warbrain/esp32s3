#include "app/services.hpp"
#include "base/project_paths.hpp"

#include <cstdio>
#include <filesystem>
#include <chrono>

namespace hmi::app
{
    namespace
    {
    std::string path_utf8(const std::filesystem::path &path)
    {
        const auto text = path.generic_u8string();
        return {reinterpret_cast<const char *>(text.data()), text.size()};
    }

    std::string wide_utf8(const std::wstring &value)
    {
        const auto text = std::filesystem::path(value).u8string();
        return {reinterpret_cast<const char *>(text.data()), text.size()};
    }

    double elapsed_ms(std::chrono::steady_clock::time_point started)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    }

    Services::Services() : voice_(&logger_) {}
    Services::~Services() { shutdown(); }

    void Services::prepare()
    {
        if (prepared_) return;
        prepared_ = true;
        configuration_error_.clear();
        logging_warning_.clear();
        tts_setting_error_.clear();
        microphone_selection_observed_ = false;
        microphone_refreshes_ = camera_refreshes_ = 0;
        log_flush_elapsed_seconds_ = 0.0;
        const auto base_directory = executable_directory();
        auto file = settings_path();
        if (!base::resolve_project_path(file, base_directory, file, &configuration_error_))
            configuration_error_ = "Application configuration rejected: " + configuration_error_;
        configuration_file_ = path_utf8(file);
        settings_ = default_settings(base_directory);
        std::error_code code;
        if (configuration_error_.empty() && !std::filesystem::exists(file, code) && !code)
        {
            // Migrate the old shortcut file only when no YAML file exists.
            base::Config legacy;
            std::string ignored;
            if (legacy.load(base_directory / L"config/hotkeys.json", &ignored))
            {
                settings_.voice.hotkeys.hold = legacy.get<std::string>("hold", settings_.voice.hotkeys.hold);
                settings_.voice.hotkeys.toggle = legacy.get<std::string>("toggle", settings_.voice.hotkeys.toggle);
            }
            if (!write_default_settings(file, settings_, base_directory, &configuration_error_))
                configuration_error_ = "Cannot create application configuration: " + configuration_error_;
        }
        if (configuration_error_.empty() && (!configuration_.load(file, &configuration_error_) ||
            !read_settings(configuration_, base_directory, settings_, &configuration_error_)))
            configuration_error_ = "Application configuration rejected: " + configuration_error_;
        const bool safe_log = validate_settings_paths(settings_, file, base_directory, &logging_warning_);
        if (!safe_log)
        {
            configuration_error_ = "Application configuration rejected: " + logging_warning_;
            logger_.shutdown();
        }
        else if (!logger_.configure(settings_.logging, &logging_warning_))
            std::fprintf(stderr, "HMI logging unavailable: %s\n", logging_warning_.c_str());
        logger_.write(base::LogLevel::Info, "application", "starting; config=" + configuration_file_);
        if (!configuration_error_.empty())
        {
            logger_.write(base::LogLevel::Error, "configuration", configuration_error_);
            return;
        }
        logger_.write(base::LogLevel::Info, "configuration", "loaded; hold=" + settings_.voice.hotkeys.hold +
            " toggle=" + settings_.voice.hotkeys.toggle);
        logger_.flush();
    }

    void Services::start()
    {
        prepare();
        if (started_ || !configuration_error_.empty()) return;
        started_ = true;
        refresh_microphones();
        refresh_cameras();
        std::string error;
        if (!voice_.start(settings_.voice, &error)) logger_.write(base::LogLevel::Error, "voice", error);
        logger_.flush();
    }

    void Services::refresh_cameras()
    {
        const auto started = std::chrono::steady_clock::now();
        ++camera_refreshes_;
        cameras_ = camera::enumerate_devices();
        logger_.write(base::LogLevel::Info, "devices", "event=camera_refresh refresh_count=" +
            std::to_string(camera_refreshes_) + " device_count=" + std::to_string(cameras_.size()) +
            " wall_ms=" + std::to_string(elapsed_ms(started)) + " enumeration_status=unreported");
    }

    void Services::refresh_microphones()
    {
        const auto started = std::chrono::steady_clock::now();
        ++microphone_refreshes_;
        const bool ok = microphones_.refresh();
        logger_.write(ok ? base::LogLevel::Info : base::LogLevel::Warning, "devices",
            "event=microphone_refresh refresh_count=" + std::to_string(microphone_refreshes_) +
            " device_count=" + std::to_string(microphones_.devices().size()) +
            " result=" + (ok ? "complete" : "failed") + " wall_ms=" + std::to_string(elapsed_ms(started)));
        observe_microphone_selection();
    }

    void Services::observe_microphone_selection()
    {
        const auto &selected = microphones_.selected_id();
        if (microphone_selection_observed_ && selected == observed_microphone_id_) return;
        const auto previous = wide_utf8(observed_microphone_id_);
        observed_microphone_id_ = selected;
        microphone_selection_observed_ = true;
        std::string name;
        for (const auto &device : microphones_.devices())
            if (device.id == selected) { name = wide_utf8(device.name); break; }
        logger_.write(base::LogLevel::Info, "devices", "event=microphone_selection_changed previous_id=" +
            previous + " selected_id=" + wide_utf8(selected) + " name=" + name + " selection_scope=app_local");
    }

    void Services::poll(float delta_seconds)
    {
        if (started_ && configuration_error_.empty()) voice_.tick();
        if (started_) observe_microphone_selection();
        if (!(delta_seconds > 0.0f))
            return;

        log_flush_elapsed_seconds_ += delta_seconds;
        if (log_flush_elapsed_seconds_ >= 2.0)
        {
            logger_.flush();
            log_flush_elapsed_seconds_ = 0.0;
        }
    }

    void Services::shutdown()
    {
        if (!prepared_) return;
        voice_.stop();
        logger_.write(base::LogLevel::Info, "application", "shutdown complete");
        logger_.write_statistics();
        logger_.shutdown();
        started_ = false;
        prepared_ = false;
    }

    VoiceSnapshot Services::voice_snapshot() const
    {
        auto snapshot = voice_.snapshot();
        if (!configuration_error_.empty())
        {
            snapshot.state = VoiceState::Error;
            snapshot.status = configuration_error_;
        }
        if (!logging_warning_.empty()) snapshot.status += "; logging configuration warning: " + logging_warning_;
        snapshot.tts_toggle_error = tts_setting_error_;
        return snapshot;
    }

    bool Services::set_tts_enabled(bool enabled, std::string *error)
    {
        tts_setting_error_.clear();
        std::string detail;
        const auto selected = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t *>(configuration_file_.data()), configuration_file_.size()));
        // Read a detached transaction so unrelated edits on disk are retained.
        if (!configuration_error_.empty() || !save_tts_enabled(selected, enabled, executable_directory(), &detail))
        {
            tts_setting_error_ = "开关未保存，请检查配置文件是否可写。";
            if (error) *error = tts_setting_error_;
            logger_.write(base::LogLevel::Warning, "configuration", "TTS switch unchanged; detail=" + detail);
            return false;
        }
        settings_.voice.tts_enabled = enabled;
        voice_.set_tts_enabled(enabled);
        if (error) error->clear();
        return true;
    }
}
