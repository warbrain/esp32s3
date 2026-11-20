#include "check.hpp"
#include "base/project_paths.hpp"
#include "app/settings.hpp"

HMI_TEST(project_paths_resolve_local_relative_and_missing_outputs)
{
    const auto directory = hmi::base::process_directory();
    std::filesystem::path resolved;
    std::string error;
    CHECK(hmi::base::resolve_project_path("logs/future.log", directory, resolved, &error));
    CHECK(resolved == std::filesystem::weakly_canonical(directory / "logs/future.log"));
    CHECK(error.empty());
}

HMI_TEST(project_paths_reject_parent_traversal_and_similar_prefix)
{
    const auto directory = hmi::base::process_directory();
    const auto root = hmi::base::project_root(directory);
    std::filesystem::path resolved;
    std::string error;
    CHECK(!hmi::base::resolve_project_path(root / "../outside-models/asr.gguf", directory, resolved, &error));
    CHECK(error.find("inside the project") != std::string::npos);
    CHECK(!hmi::base::resolve_project_path(root.native() + L"-other/model.gguf", directory, resolved));
    auto nul = (root / "logs/hmi.log").native();
    nul.push_back(L'\0');
    nul += L"ignored";
    CHECK(!hmi::base::resolve_project_path(std::filesystem::path(nul), directory, resolved));
}

HMI_TEST(application_settings_reject_external_configuration_and_data_paths)
{
    const auto directory = hmi::base::process_directory();
    const auto file = directory / "config/app.yaml";
    const auto outside = hmi::base::project_root(directory).parent_path() / "outside-project/data";
    const auto defaults = hmi::app::default_settings(directory);
    std::string error;
    CHECK(hmi::app::validate_settings_paths(defaults, file, directory, &error));
    CHECK(!hmi::app::validate_settings_paths(defaults, outside, directory, &error));
    auto selected = defaults;
    selected.logging.file = outside;
    CHECK(!hmi::app::validate_settings_paths(selected, file, directory, &error));
    selected = defaults;
    selected.voice.asr_model = outside;
    CHECK(!hmi::app::validate_settings_paths(selected, file, directory, &error));
    selected = defaults;
    selected.voice.tts_model = outside;
    CHECK(!hmi::app::validate_settings_paths(selected, file, directory, &error));
    selected = defaults;
    selected.voice.rules_file = outside;
    CHECK(!hmi::app::validate_settings_paths(selected, file, directory, &error));
}
