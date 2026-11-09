#include "app/settings.hpp"
#include "base/config.hpp"
#include "base/project_paths.hpp"

#include <windows.h>

#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace hmi::app
{
namespace
{
std::string utf8(const std::filesystem::path &path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

std::filesystem::path native_path(const std::string &text, const std::filesystem::path &base_directory)
{
    if (text.find('\0') != std::string::npos) throw std::runtime_error("configuration path contains NUL");
    const std::filesystem::path path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
    return path.is_absolute() ? path.lexically_normal() : (base_directory / path).lexically_normal();
}

std::filesystem::path comparable_path(const std::filesystem::path &path)
{
    std::error_code code;
    auto resolved = std::filesystem::weakly_canonical(path, code);
    // A first launch may not have the log/config parent directories yet.
    return code ? std::filesystem::absolute(path).lexically_normal() : resolved;
}

template<class T>
void read(const base::Config &config, const char *key, T &value)
{
    if (!config.value(key).IsDefined()) return;
    std::string detail;
    if (!config.try_get(key, value, &detail)) throw std::runtime_error(std::string(key) + ": " + detail);
}
}

std::filesystem::path executable_directory()
{
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, 32768);
    if (!count || count == 32768) throw std::runtime_error("cannot locate application directory");
    return std::filesystem::path(std::wstring(path, count)).parent_path();
}

std::filesystem::path settings_path()
{
    wchar_t *value = nullptr;
    std::size_t size = 0;
    _wdupenv_s(&value, &size, L"HMI_CONFIG");
    const std::unique_ptr<wchar_t, decltype(&std::free)> owner(value, &std::free);
    return value && *value ? std::filesystem::path(value) : executable_directory() / L"config/app.yaml";
}

Settings default_settings(const std::filesystem::path &base_directory)
{
    Settings settings;
    settings.voice.tts_prewarm = false;
    settings.voice.vad_model = base::project_root(base_directory) / L"models/frontend/silero_vad_16k.safetensors";
    settings.voice.hgtcrn_model = base::project_root(base_directory) / L"models/frontend/hgtcrn_noisy.safetensors";
    settings.voice.hotkeys.hold = "Ctrl+Alt";
    settings.voice.hotkeys.use_config_file = false;
    settings.logging.file = base_directory / L"logs/hmi.log";
    return settings;
}

bool validate_settings_paths(const Settings &settings, const std::filesystem::path &configuration_file,
                             const std::filesystem::path &base_directory, std::string *error)
{
    try
    {
        std::filesystem::path local;
        for (const auto &input : {configuration_file, settings.logging.file, settings.voice.rules_file,
                                  settings.voice.asr_model, settings.voice.tts_model,
                                  settings.voice.vad_model, settings.voice.denoise_model, settings.voice.hgtcrn_model})
        {
            if (!input.empty() && !base::resolve_project_path(input, base_directory, local, error)) return false;
        }
        const auto log = comparable_path(settings.logging.file);
        const auto rules = settings.voice.rules_file.empty() ? base_directory / L"rules/voice.json" : settings.voice.rules_file;
        for (const auto &input : {configuration_file, rules, settings.voice.asr_model, settings.voice.tts_model,
                                  settings.voice.vad_model, settings.voice.denoise_model, settings.voice.hgtcrn_model})
        {
            if (input.empty()) continue;
            const auto source = comparable_path(input);
            std::error_code code;
            const bool same_file = std::filesystem::equivalent(log, source, code);
            if (same_file || CompareStringOrdinal(log.c_str(), -1, source.c_str(), -1, TRUE) == CSTR_EQUAL)
                throw std::runtime_error("logging.file must differ from configuration, rules, and model files");
        }
        if (error) error->clear();
        return true;
    }
    catch (const std::exception &exception)
    {
        if (error) *error = exception.what();
        return false;
    }
}

bool read_settings(const base::Config &config, const std::filesystem::path &base_directory,
                   Settings &out, std::string *error)
{
    try
    {
        Settings parsed = default_settings(base_directory);
        int schema = 1;
        read(config, "schema", schema);
        if (schema != 1) throw std::runtime_error("unsupported application configuration schema");
        for (const char *section : {"voice", "hotkeys", "rules", "logging"})
        {
            const auto node = config.value(section);
            if (node.IsDefined() && !node.IsMap()) throw std::runtime_error(std::string(section) + " must be a mapping");
        }
        read(config, "voice.backend", parsed.voice.backend);
        if (parsed.voice.backend != "auto" && parsed.voice.backend != "cpu" &&
            parsed.voice.backend != "cuda" && parsed.voice.backend != "vulkan")
            throw std::runtime_error("voice.backend must be auto, cpu, cuda, or vulkan");
        read(config, "voice.threads", parsed.voice.threads);
        if (parsed.voice.threads < 1 || parsed.voice.threads > 256)
            throw std::runtime_error("voice.threads must be 1..256");
        read(config, "voice.tts_enabled", parsed.voice.tts_enabled);
        read(config, "voice.tts_prewarm", parsed.voice.tts_prewarm);
        read(config, "voice.vad_enabled", parsed.voice.vad_enabled);
        read(config, "voice.denoise_enabled", parsed.voice.denoise_enabled);
        read(config, "voice.hgtcrn_enabled", parsed.voice.hgtcrn_enabled);
        read(config, "voice.vad_threshold", parsed.voice.vad_threshold);
        if (!std::isfinite(parsed.voice.vad_threshold) || parsed.voice.vad_threshold <= 0.0f || parsed.voice.vad_threshold >= 1.0f)
            throw std::runtime_error("voice.vad_threshold must be greater than zero and less than one");
        read(config, "voice.preview_interval_seconds", parsed.voice.preview_interval_seconds);
        if (!std::isfinite(parsed.voice.preview_interval_seconds) || parsed.voice.preview_interval_seconds < 1.0 ||
            parsed.voice.preview_interval_seconds > 10.0)
            throw std::runtime_error("voice.preview_interval_seconds must be 1..10 seconds");
        read(config, "voice.refine", parsed.voice.refine);
        read(config, "voice.maximum_audio_seconds", parsed.voice.maximum_audio_seconds);
        if (parsed.voice.maximum_audio_seconds < 1 || parsed.voice.maximum_audio_seconds > 30)
            throw std::runtime_error("voice.maximum_audio_seconds must be 1..30");
        read(config, "audio.source", parsed.voice.audio_source.source);
        if (parsed.voice.audio_source.source != "local" && parsed.voice.audio_source.source != "esp32")
            throw std::runtime_error("audio.source must be \"local\" or \"esp32\"");
        read(config, "audio.esp32_host", parsed.voice.audio_source.esp32_host);
        if (parsed.voice.audio_source.esp32_host.empty() || parsed.voice.audio_source.esp32_host.size() > 64)
            throw std::runtime_error("audio.esp32_host must contain 1..64 characters");
        read(config, "audio.esp32_port", parsed.voice.audio_source.esp32_port);
        read(config, "audio.volume", parsed.voice.audio_source.volume);
        if (parsed.voice.audio_source.volume < 0 || parsed.voice.audio_source.volume > 100)
            throw std::runtime_error("audio.volume must be 0..100");
        if (parsed.voice.audio_source.esp32_port < 1 || parsed.voice.audio_source.esp32_port > 65535)
            throw std::runtime_error("audio.esp32_port must be 1..65535");
        read(config, "hotkeys.hold", parsed.voice.hotkeys.hold);
        read(config, "hotkeys.toggle", parsed.voice.hotkeys.toggle);
        if (parsed.voice.hotkeys.hold.empty() || parsed.voice.hotkeys.toggle.empty() ||
            parsed.voice.hotkeys.hold.size() > 128 || parsed.voice.hotkeys.toggle.size() > 128)
            throw std::runtime_error("hotkey bindings must contain 1..128 characters");
        std::string asr, tts, vad, denoise, hgtcrn, rules;
        read(config, "voice.asr_model", asr);
        read(config, "voice.tts_model", tts);
        read(config, "voice.vad_model", vad);
        read(config, "voice.denoise_model", denoise);
        read(config, "voice.hgtcrn_model", hgtcrn);
        read(config, "rules.file", rules);
        if (!asr.empty()) parsed.voice.asr_model = native_path(asr, base_directory);
        if (!tts.empty()) parsed.voice.tts_model = native_path(tts, base_directory);
        if (!vad.empty()) parsed.voice.vad_model = native_path(vad, base_directory);
        if (!denoise.empty()) parsed.voice.denoise_model = native_path(denoise, base_directory);
        if (!hgtcrn.empty()) parsed.voice.hgtcrn_model = native_path(hgtcrn, base_directory);
        if (!rules.empty()) parsed.voice.rules_file = native_path(rules, base_directory);
        std::string level = "info", file = "logs/hmi.log";
        std::int64_t megabytes = 5;
        int files = 3;
        read(config, "logging.level", level);
        read(config, "logging.file", file);
        read(config, "logging.max_size_mb", megabytes);
        read(config, "logging.max_files", files);
        read(config, "logging.console", parsed.logging.console);
        if (!base::parse_log_level(level, parsed.logging.level)) throw std::runtime_error("invalid logging.level");
        if (file.empty()) throw std::runtime_error("logging.file must not be empty");
        if (megabytes < 1 || megabytes > 1024) throw std::runtime_error("logging.max_size_mb must be 1..1024");
        if (files < 1 || files > 32) throw std::runtime_error("logging.max_files must be 1..32");
        parsed.logging.file = native_path(file, base_directory);
        parsed.logging.max_bytes = static_cast<std::size_t>(megabytes) * 1024 * 1024;
        parsed.logging.max_files = static_cast<std::size_t>(files);
        out = std::move(parsed);
        if (error) error->clear();
        return true;
    }
    catch (const std::exception &failure)
    {
        if (error) *error = failure.what();
        return false;
    }
}

bool write_default_settings(const std::filesystem::path &file, const Settings &settings,
                            const std::filesystem::path &base_directory, std::string *error)
{
    base::Config config;
    const auto set = [&config, error](const char *key, const auto &value) { return config.set(key, value, error); };
    const auto relative = settings.logging.file.lexically_relative(base_directory);
    if (!set("schema", 1) || !set("voice.backend", settings.voice.backend) ||
        !set("voice.threads", settings.voice.threads) || !set("voice.tts_enabled", settings.voice.tts_enabled) ||
        !set("voice.tts_prewarm", settings.voice.tts_prewarm) ||
        !set("voice.vad_enabled", settings.voice.vad_enabled) || !set("voice.denoise_enabled", settings.voice.denoise_enabled) ||
        !set("voice.hgtcrn_enabled", settings.voice.hgtcrn_enabled) ||
        !set("voice.vad_threshold", settings.voice.vad_threshold) ||
        !set("voice.preview_interval_seconds", settings.voice.preview_interval_seconds) ||
        !set("voice.refine", settings.voice.refine) || !set("voice.maximum_audio_seconds", settings.voice.maximum_audio_seconds) ||
        !set("voice.asr_model", utf8(settings.voice.asr_model)) || !set("voice.tts_model", utf8(settings.voice.tts_model)) ||
        !set("voice.vad_model", utf8(settings.voice.vad_model)) || !set("voice.denoise_model", utf8(settings.voice.denoise_model)) ||
        !set("voice.hgtcrn_model", utf8(settings.voice.hgtcrn_model)) ||
        !set("rules.file", settings.voice.rules_file.empty() ? std::string("rules/voice.json") : utf8(settings.voice.rules_file)) || !set("audio.source", settings.voice.audio_source.source) || !set("audio.esp32_host", settings.voice.audio_source.esp32_host) || !set("audio.esp32_port", settings.voice.audio_source.esp32_port) || !set("audio.volume", settings.voice.audio_source.volume) || !set("hotkeys.hold", settings.voice.hotkeys.hold) ||
        !set("hotkeys.toggle", settings.voice.hotkeys.toggle) || !set("logging.level", std::string(base::log_level_name(settings.logging.level))) ||
        !set("logging.file", utf8(relative.empty() ? settings.logging.file : relative)) ||
        !set("logging.max_size_mb", settings.logging.max_bytes / (1024 * 1024)) ||
        !set("logging.max_files", settings.logging.max_files) || !set("logging.console", settings.logging.console)) return false;
    return config.save(file, error);
}

bool save_tts_enabled(const std::filesystem::path &file, bool enabled,
                      const std::filesystem::path &base_directory, std::string *error)
{
    std::filesystem::path local;
    if (!base::resolve_project_path(file, base_directory, local, error)) return false;
    base::Config changed;
    Settings parsed;
    // Keep unrelated keys from the latest disk version and commit atomically.
    return changed.load(local, error) && read_settings(changed, base_directory, parsed, error) &&
        validate_settings_paths(parsed, local, base_directory, error) &&
        changed.set("voice.tts_enabled", enabled, error) && changed.save(local, error);
}
}
