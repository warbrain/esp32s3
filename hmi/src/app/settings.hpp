#pragma once

#include "app/voice_runtime.hpp"
#include "base/logger.hpp"

#include <filesystem>
#include <string>

namespace hmi::base { class Config; }

namespace hmi::app
{
struct Settings
{
    VoiceOptions voice;
    base::LoggerOptions logging;
};

std::filesystem::path executable_directory();
std::filesystem::path settings_path();
Settings default_settings(const std::filesystem::path &base_directory);
bool read_settings(const base::Config &config, const std::filesystem::path &base_directory,
                   Settings &out, std::string *error = nullptr);
bool validate_settings_paths(const Settings &settings, const std::filesystem::path &configuration_file,
                             const std::filesystem::path &base_directory, std::string *error = nullptr);
bool write_default_settings(const std::filesystem::path &file, const Settings &settings,
                            const std::filesystem::path &base_directory, std::string *error = nullptr);
bool save_tts_enabled(const std::filesystem::path &file, bool enabled,
                      const std::filesystem::path &base_directory, std::string *error = nullptr);
}
