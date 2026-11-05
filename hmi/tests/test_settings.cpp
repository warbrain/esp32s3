#include "check.hpp"
#include "app/settings.hpp"
#include "base/config.hpp"
#include "base/logger.hpp"
#include "base/project_paths.hpp"
#include "intent/rule_io.hpp"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class SettingsDirectory
{
public:
    SettingsDirectory()
    {
        parent_ = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
        path = parent_ / (L"hmi-settings-中文-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~SettingsDirectory()
    {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        const auto base = std::filesystem::weakly_canonical(parent_, error);
        if (!error && resolved.parent_path() == base && resolved.filename().native().starts_with(L"hmi-settings-"))
            std::filesystem::remove_all(resolved, error);
    }

    std::filesystem::path path;

private:
    std::filesystem::path parent_;
};

class ScopedSettingsEnvironment
{
public:
    ScopedSettingsEnvironment(const wchar_t *name, const std::filesystem::path &value) : name_(name)
    {
        wchar_t *old = nullptr;
        std::size_t size = 0;
        if (_wdupenv_s(&old, &size, name) == 0 && old) previous_ = old;
        std::free(old);
        _wputenv_s(name_.c_str(), value.c_str());
    }
    ~ScopedSettingsEnvironment() { _wputenv_s(name_.c_str(), previous_.c_str()); }

private:
    std::wstring name_;
    std::wstring previous_;
};

std::string utf8(const std::filesystem::path &path)
{
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

bool same_settings(const hmi::app::Settings &left, const hmi::app::Settings &right)
{
    return left.voice.backend == right.voice.backend && left.voice.threads == right.voice.threads &&
        left.voice.tts_enabled == right.voice.tts_enabled &&
        left.voice.tts_prewarm == right.voice.tts_prewarm && left.voice.asr_model == right.voice.asr_model &&
        left.voice.vad_enabled == right.voice.vad_enabled && left.voice.denoise_enabled == right.voice.denoise_enabled &&
        left.voice.hgtcrn_enabled == right.voice.hgtcrn_enabled && left.voice.hgtcrn_model == right.voice.hgtcrn_model &&
        left.voice.vad_threshold == right.voice.vad_threshold && left.voice.vad_model == right.voice.vad_model &&
        left.voice.denoise_model == right.voice.denoise_model &&
        left.voice.preview_interval_seconds == right.voice.preview_interval_seconds &&
        left.voice.refine == right.voice.refine && left.voice.maximum_audio_seconds == right.voice.maximum_audio_seconds &&
        left.voice.tts_model == right.voice.tts_model && left.voice.rules_file == right.voice.rules_file &&
        left.voice.hotkeys.hold == right.voice.hotkeys.hold && left.voice.hotkeys.toggle == right.voice.hotkeys.toggle &&
        left.voice.hotkeys.use_config_file == right.voice.hotkeys.use_config_file &&
        left.logging.file == right.logging.file && left.logging.level == right.logging.level &&
        left.logging.max_bytes == right.logging.max_bytes && left.logging.max_files == right.logging.max_files &&
        left.logging.console == right.logging.console;
}

std::string read_text(const std::filesystem::path &file)
{
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

HMI_TEST(settings_tts_switch_persists_without_overwriting_other_configuration)
{
    SettingsDirectory directory;
    const auto file = directory.path / L"app.yaml";
    auto settings = hmi::app::default_settings(directory.path);
    std::string error;
    CHECK(hmi::app::write_default_settings(file, settings, directory.path, &error));
    hmi::base::Config edited;
    CHECK(edited.load(file, &error));
    CHECK(edited.set("metadata.note", "保留此值"));
    CHECK(edited.set("voice.threads", 7));
    CHECK(edited.save(file, &error));
    CHECK(hmi::app::save_tts_enabled(file, true, directory.path, &error));
    CHECK(edited.load(file, &error));
    CHECK(edited.get<bool>("voice.tts_enabled", false));
    CHECK(edited.get<int>("voice.threads", 0) == 7);
    CHECK(edited.get<std::string>("metadata.note", "") == "保留此值");
    CHECK(hmi::app::read_settings(edited, directory.path, settings, &error));
    CHECK(settings.voice.tts_enabled);
    CHECK(hmi::app::save_tts_enabled(file, false, directory.path, &error));
    CHECK(edited.load(file, &error));
    CHECK(!edited.get<bool>("voice.tts_enabled", true));

    const auto before = read_text(file);
    CHECK(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    const bool saved = hmi::app::save_tts_enabled(file, true, directory.path, &error);
    CHECK(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE);
    CHECK(!saved && !error.empty());
    CHECK(read_text(file) == before);
}

HMI_TEST(settings_defaults_and_empty_yaml_mapping)
{
    SettingsDirectory directory;
    hmi::base::Config config;
    auto settings = hmi::app::default_settings(directory.path);
    CHECK(settings.voice.backend == "auto");
    CHECK(settings.voice.threads == 4);
    CHECK(!settings.voice.tts_prewarm);
    CHECK(!settings.voice.tts_enabled);
    CHECK(settings.voice.vad_enabled && !settings.voice.denoise_enabled);
    CHECK(!settings.voice.hgtcrn_enabled);
    CHECK(settings.voice.vad_threshold == 0.35f);
    const auto project = hmi::base::project_root(directory.path);
    CHECK(settings.voice.vad_model == project / L"models/frontend/silero_vad_16k.safetensors");
    CHECK(settings.voice.denoise_model.empty());
    CHECK(settings.voice.hgtcrn_model == project / L"models/frontend/hgtcrn_noisy.safetensors");
    CHECK(settings.voice.preview_interval_seconds == 1.0);
    CHECK(settings.voice.refine && settings.voice.maximum_audio_seconds == 30);
    CHECK(settings.voice.hotkeys.hold == "Ctrl+Alt");
    CHECK(settings.voice.hotkeys.toggle == "Ctrl+Shift+M");
    CHECK(!settings.voice.hotkeys.use_config_file);
    CHECK(settings.logging.file == directory.path / L"logs/hmi.log");
    CHECK(settings.logging.level == hmi::base::LogLevel::Info);
    std::string error = "old error";
    CHECK(hmi::app::read_settings(config, directory.path, settings, &error));
    CHECK(error.empty());
    CHECK(same_settings(settings, hmi::app::default_settings(directory.path)));
}

HMI_TEST(settings_validate_boundaries_and_leave_previous_options_on_failure)
{
    SettingsDirectory directory;
    auto previous = hmi::app::default_settings(directory.path);
    previous.voice.backend = "vulkan";
    previous.voice.threads = 7;
    previous.logging.level = hmi::base::LogLevel::Debug;
    previous.logging.file = directory.path / L"previous.log";
    const std::vector<std::pair<std::string, YAML::Node>> invalid{
        {"schema", YAML::Node(2)}, {"voice.backend", YAML::Node("invalid")},
        {"voice.threads", YAML::Node(0)}, {"voice.threads", YAML::Node(257)},
        {"voice.threads", YAML::Node("many")}, {"voice.tts_prewarm", YAML::Node("maybe")},
        {"voice.tts_enabled", YAML::Node("maybe")},
        {"voice.vad_enabled", YAML::Node("maybe")}, {"voice.denoise_enabled", YAML::Node("maybe")},
        {"voice.hgtcrn_enabled", YAML::Node("maybe")},
        {"voice.vad_threshold", YAML::Node(0.0)}, {"voice.vad_threshold", YAML::Node(1.0)},
        {"voice.vad_threshold", YAML::Node(".nan")}, {"voice.vad_threshold", YAML::Node(".inf")},
        {"voice.preview_interval_seconds", YAML::Node(0.5)}, {"voice.preview_interval_seconds", YAML::Node(11.0)},
        {"voice.refine", YAML::Node("maybe")}, {"voice.maximum_audio_seconds", YAML::Node(0)},
        {"voice.maximum_audio_seconds", YAML::Node(31)},
        {"logging.level", YAML::Node("verbose-invalid")}, {"logging.file", YAML::Node("")},
        {"logging.max_size_mb", YAML::Node(0)}, {"logging.max_size_mb", YAML::Node(1025)},
        {"logging.max_files", YAML::Node(0)}, {"logging.max_files", YAML::Node(33)},
        {"hotkeys.hold", YAML::Node("")}, {"hotkeys.toggle", YAML::Node(std::string(129, 'A'))},
        {"voice.asr_model", YAML::Node(std::string("bad\0path", 8))},
        {"voice.vad_model", YAML::Node(std::string("bad\0path", 8))},
        {"voice.denoise_model", YAML::Node(std::string("bad\0path", 8))},
        {"voice.hgtcrn_model", YAML::Node(std::string("bad\0path", 8))}
    };
    for (const auto &[key, value] : invalid)
    {
        hmi::base::Config config;
        CHECK(config.set_node(key, value));
        auto settings = previous;
        std::string error;
        CHECK(!hmi::app::read_settings(config, directory.path, settings, &error));
        CHECK(!error.empty());
        CHECK(same_settings(settings, previous));
    }
    for (const int threads : {1, 256})
    {
        hmi::base::Config config;
        CHECK(config.set("voice.threads", threads));
        CHECK(config.set("logging.max_size_mb", threads == 1 ? 1 : 1024));
        CHECK(config.set("logging.max_files", threads == 1 ? 1 : 32));
        auto settings = previous;
        CHECK(hmi::app::read_settings(config, directory.path, settings));
        CHECK(settings.voice.threads == threads);
        CHECK(settings.logging.max_bytes == static_cast<std::size_t>(threads == 1 ? 1 : 1024) * 1024 * 1024);
    }
}

HMI_TEST(settings_reject_malformed_sections_instead_of_silently_using_defaults)
{
    SettingsDirectory directory;
    for (const std::string_view section : {"voice", "hotkeys", "rules", "logging"})
    {
        for (const auto &value : {YAML::Node(17), YAML::Node(YAML::NodeType::Sequence), YAML::Node(YAML::NodeType::Null)})
        {
            hmi::base::Config config;
            CHECK(config.set_node(section, value));
            auto settings = hmi::app::default_settings(directory.path);
            const auto previous = settings;
            std::string error;
            CHECK(!hmi::app::read_settings(config, directory.path, settings, &error));
            CHECK(!error.empty());
            CHECK(same_settings(settings, previous));
        }
    }
}

HMI_TEST(settings_resolve_unicode_paths_and_yaml_hotkeys_from_application_directory)
{
    SettingsDirectory directory;
    const auto base = directory.path / L"程序";
    hmi::base::Config config;
    CHECK(config.set("voice.backend", "cuda"));
    CHECK(config.set("voice.tts_prewarm", true));
    CHECK(config.set("voice.asr_model", utf8(L"../模型/识别.gguf")));
    CHECK(config.set("voice.tts_model", utf8(L"模型/合成.gguf")));
    CHECK(config.set("voice.vad_model", utf8(L"模型/静音检测.safetensors")));
    CHECK(config.set("voice.denoise_model", utf8(L"模型/降噪.safetensors")));
    CHECK(config.set("voice.hgtcrn_model", utf8(L"模型/双通道降噪.safetensors")));
    CHECK(config.set("rules.file", utf8(L"规则/语音.json")));
    CHECK(config.set("logging.file", utf8(L"日志/运行.log")));
    CHECK(config.set("logging.level", "warning"));
    CHECK(config.set("hotkeys.hold", "Alt+Space"));
    CHECK(config.set("hotkeys.toggle", "Ctrl+Alt+M"));
    hmi::app::Settings settings;
    CHECK(hmi::app::read_settings(config, base, settings));
    CHECK(settings.voice.tts_prewarm);
    CHECK(settings.voice.asr_model == (base / L"../模型/识别.gguf").lexically_normal());
    CHECK(settings.voice.tts_model == base / L"模型/合成.gguf");
    CHECK(settings.voice.vad_model == base / L"模型/静音检测.safetensors");
    CHECK(settings.voice.denoise_model == base / L"模型/降噪.safetensors");
    CHECK(settings.voice.hgtcrn_model == base / L"模型/双通道降噪.safetensors");
    CHECK(settings.voice.rules_file == base / L"规则/语音.json");
    CHECK(settings.logging.file == base / L"日志/运行.log");
    CHECK(settings.logging.level == hmi::base::LogLevel::Warning);
    CHECK(settings.voice.hotkeys.hold == "Alt+Space");
    CHECK(settings.voice.hotkeys.toggle == "Ctrl+Alt+M");
    CHECK(!settings.voice.hotkeys.use_config_file);
    const auto absolute = directory.path / L"绝对模型.gguf";
    CHECK(config.set("voice.asr_model", utf8(absolute)));
    CHECK(hmi::app::read_settings(config, base, settings));
    CHECK(settings.voice.asr_model == absolute);
}

HMI_TEST(settings_default_file_and_custom_options_roundtrip)
{
    SettingsDirectory directory;
    const auto base = directory.path / L"应用";
    const auto file = base / L"config/应用.yaml";
    auto written = hmi::app::default_settings(base);
    std::string error;
    CHECK(hmi::app::write_default_settings(file, written, base, &error));
    hmi::base::Config config;
    CHECK(config.load(file, &error));
    hmi::app::Settings read;
    CHECK(hmi::app::read_settings(config, base, read, &error));
    CHECK(read.voice.backend == written.voice.backend);
    CHECK(read.voice.threads == written.voice.threads);
    CHECK(read.voice.rules_file == base / L"rules/voice.json");
    CHECK(read.logging.file == written.logging.file);
    CHECK(read.logging.max_bytes == written.logging.max_bytes);
    CHECK(!read.voice.hotkeys.use_config_file);
    written.voice.backend = "cpu";
    written.voice.threads = 3;
    written.voice.tts_prewarm = false;
    written.voice.preview_interval_seconds = 3.0;
    written.voice.refine = false;
    written.voice.maximum_audio_seconds = 20;
    written.voice.vad_enabled = false;
    written.voice.denoise_enabled = true;
    written.voice.hgtcrn_enabled = true;
    written.voice.vad_threshold = 0.6f;
    written.voice.vad_model = base / L"模型/静音检测.safetensors";
    written.voice.denoise_model = base / L"模型/降噪.safetensors";
    written.voice.hgtcrn_model = base / L"模型/双通道降噪.safetensors";
    written.voice.asr_model = base / L"模型/识别.gguf";
    written.voice.tts_model = base / L"模型/合成.gguf";
    written.voice.rules_file = base / L"规则/自定义.json";
    written.voice.hotkeys.hold = "Alt+Space";
    written.voice.hotkeys.toggle = "Ctrl+Alt+M";
    written.logging.file = base / L"日志/应用.log";
    written.logging.level = hmi::base::LogLevel::Warning;
    written.logging.max_bytes = 9 * 1024 * 1024;
    written.logging.max_files = 5;
    CHECK(hmi::app::write_default_settings(file, written, base, &error));
    CHECK(config.load(file, &error));
    CHECK(hmi::app::read_settings(config, base, read, &error));
    CHECK(same_settings(read, written));
}

HMI_TEST(settings_log_path_cannot_overwrite_configuration_rules_or_models)
{
    SettingsDirectory directory;
    const auto file = directory.path / L"config/app.yaml";
    auto settings = hmi::app::default_settings(directory.path);
    std::string error;
    const bool valid_default = hmi::app::validate_settings_paths(settings, file, directory.path, &error);
    if (!valid_default) std::fprintf(stderr, "settings path validation: %s\n", error.c_str());
    CHECK(valid_default);
    for (const auto &input : {file, directory.path / L"rules/voice.json"})
    {
        settings.logging.file = input;
        CHECK(!hmi::app::validate_settings_paths(settings, file, directory.path, &error));
        CHECK(error.find("logging.file") != std::string::npos);
    }
    settings.logging.file = directory.path / L"CONFIG/APP.YAML";
    CHECK(!hmi::app::validate_settings_paths(settings, file, directory.path, &error));
    settings.voice.asr_model = directory.path / L"models/asr.gguf";
    settings.logging.file = settings.voice.asr_model;
    CHECK(!hmi::app::validate_settings_paths(settings, file, directory.path, &error));
    settings.logging.file = settings.voice.vad_model;
    CHECK(!hmi::app::validate_settings_paths(settings, file, directory.path, &error));
    settings.voice.denoise_model = directory.path / L"models/frontend/denoise.safetensors";
    settings.logging.file = settings.voice.denoise_model;
    CHECK(!hmi::app::validate_settings_paths(settings, file, directory.path, &error));
    settings.logging.file = settings.voice.hgtcrn_model;
    CHECK(!hmi::app::validate_settings_paths(settings, file, directory.path, &error));
    settings.logging.file = directory.path / L"alias.log";
    std::filesystem::create_directories(file.parent_path());
    { std::ofstream output(file, std::ios::binary); output << "schema: 1\n"; }
    std::filesystem::create_hard_link(file, settings.logging.file);
    CHECK(!hmi::app::validate_settings_paths(settings, file, directory.path, &error));
    CHECK(read_text(file) == "schema: 1\n");
}

HMI_TEST(settings_frontend_paths_stay_in_project_and_tts_save_preserves_frontend_options)
{
    SettingsDirectory directory;
    const auto file = directory.path / L"config/app.yaml";
    auto settings = hmi::app::default_settings(directory.path);
    std::string error;
    const auto project = hmi::base::project_root(directory.path);
    const auto outside = project.parent_path();
    CHECK(outside != project);
    CHECK(hmi::app::validate_settings_paths(settings, file, directory.path, &error));
    for (const int feature : {0, 1, 2})
    {
        auto invalid = settings;
        if (feature == 0) invalid.voice.vad_model = outside / L"outside-vad.safetensors";
        else if (feature == 1) invalid.voice.denoise_model = outside / L"outside-denoise.safetensors";
        else invalid.voice.hgtcrn_model = outside / L"outside-dual.safetensors";
        CHECK(!hmi::app::validate_settings_paths(invalid, file, directory.path, &error));
        CHECK(!error.empty());
    }
    CHECK(hmi::app::write_default_settings(file, settings, directory.path, &error));
    hmi::base::Config changed;
    CHECK(changed.load(file, &error));
    CHECK(changed.set("voice.vad_enabled", false));
    CHECK(changed.set("voice.denoise_enabled", true));
    CHECK(changed.set("voice.hgtcrn_enabled", false));
    CHECK(changed.set("voice.hgtcrn_model", "models/frontend/custom-dual.safetensors"));
    CHECK(changed.set("voice.denoise_model", "models/frontend/custom.safetensors"));
    CHECK(changed.set("voice.vad_threshold", 0.6));
    CHECK(changed.set("voice.frontend_note", "保留"));
    CHECK(changed.save(file, &error));
    CHECK(hmi::app::save_tts_enabled(file, true, directory.path, &error));
    CHECK(changed.load(file, &error));
    CHECK(hmi::app::read_settings(changed, directory.path, settings, &error));
    CHECK(!settings.voice.vad_enabled && settings.voice.denoise_enabled);
    CHECK(!settings.voice.hgtcrn_enabled);
    CHECK(settings.voice.hgtcrn_model == directory.path / L"models/frontend/custom-dual.safetensors");
    CHECK(settings.voice.vad_threshold == 0.6f);
    CHECK(settings.voice.denoise_model == directory.path / L"models/frontend/custom.safetensors");
    CHECK(changed.get<std::string>("voice.frontend_note", "") == "保留");
}

HMI_TEST(settings_configured_logger_records_async_voice_sdk_startup_failure)
{
    SettingsDirectory directory;
    const auto rules = directory.path / L"规则.json";
    { std::ofstream output(rules, std::ios::binary); output << hmi::intent::serialize_rules(hmi::intent::default_rules()); }
    const auto missing = directory.path / L"missing-settings-sdk.dll";
    ScopedSettingsEnvironment sdk(L"HMI_NATIVE_DLL", missing);
    ScopedSettingsEnvironment selected_rules(L"HMI_RULES", rules);
    hmi::base::Logger logger;
    hmi::base::LoggerOptions logging;
    logging.file = directory.path / L"日志/语音错误.log";
    logging.level = hmi::base::LogLevel::Error;
    std::string error;
    CHECK(logger.configure(logging, &error));
    hmi::app::VoiceRuntime runtime(&logger);
    hmi::app::VoiceOptions options;
    options.rules_file = rules;
    options.tts_prewarm = false;
    options.hotkeys.use_config_file = false;
    CHECK(runtime.start(options, &error));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    hmi::app::VoiceSnapshot snapshot;
    do
    {
        snapshot = runtime.snapshot();
        if (snapshot.state == hmi::app::VoiceState::Unavailable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    runtime.stop();
    logger.flush();
    CHECK(snapshot.state == hmi::app::VoiceState::Unavailable);
    CHECK(!snapshot.engine_ready && !snapshot.model_ready);
    CHECK(snapshot.status.find("missing-settings-sdk.dll") != std::string::npos);
    const auto log = read_text(logging.file);
    CHECK(log.find("[error]") != std::string::npos);
    CHECK(log.find("[voice]") != std::string::npos);
    CHECK(log.find("missing-settings-sdk.dll") != std::string::npos);
    logger.shutdown();
}
