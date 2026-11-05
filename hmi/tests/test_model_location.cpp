#include "check.hpp"
#include "engine/model_location.hpp"
#include "engine/model_location_detail.hpp"
#include "engine/asr_session.hpp"
#include "engine/tts_session.hpp"
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <vector>

namespace
{
struct ModelFiles
{
    std::vector<std::filesystem::path> existing;
    std::vector<std::filesystem::path> queried;

    bool operator()(const std::filesystem::path &path)
    {
        queried.push_back(path);
        return std::find(existing.begin(), existing.end(), path) != existing.end();
    }

    bool visited(const std::filesystem::path &path) const
    {
        return std::find(queried.begin(), queried.end(), path) != queried.end();
    }
};
}

HMI_TEST(model_paths_honor_project_local_override)
{
    wchar_t *previous = nullptr;
    std::size_t count = 0;
    _wdupenv_s(&previous, &count, L"HMI_MODEL_DIR");
    const std::wstring saved = previous ? previous : L"";
    std::free(previous);
    _wputenv_s(L"HMI_MODEL_DIR", L"C:/test project/models");
    const auto asr = hmi::engine::AsrConfig{}.model_path;
    const auto tts = hmi::engine::TtsConfig{}.model_path;
    _wputenv_s(L"HMI_MODEL_DIR", saved.c_str());
    CHECK(asr == "C:/test project/models/asr.gguf");
    CHECK(tts == "C:/test project/models/tts.gguf");
}

HMI_TEST(model_paths_prefer_application_weights)
{
    const std::filesystem::path root = "C:/HMI";
    const auto executable = root / "build/bin/Release";
    const auto local = executable / "models/asr.gguf";
    ModelFiles files{{local, root / "CMakeLists.txt", root / "src/main.cpp", root / "models/asr.gguf"}, {}};
    CHECK(hmi::engine::detail::select_default_model_path(executable, "asr.gguf", files) == local);
    CHECK(files.queried.size() == 1);
}

HMI_TEST(model_paths_use_recognized_project_root_only)
{
    const std::filesystem::path root = "C:/HMI";
    const auto executable = root / "build/bin/Release";
    const auto project = root / "models/asr.gguf";
    const auto intermediate = root / "build/models/asr.gguf";
    ModelFiles files{{root / "CMakeLists.txt", root / "src/main.cpp", project, intermediate}, {}};
    CHECK(hmi::engine::detail::select_default_model_path(executable, "asr.gguf", files) == project);
    CHECK(!files.visited(intermediate));
}

HMI_TEST(model_paths_do_not_escape_project_when_weights_are_missing)
{
    const std::filesystem::path outer = "C:/workspace";
    const auto root = outer / "HMI";
    const auto executable = root / "build/bin/Release";
    const auto foreign = outer / "models/asr.gguf";
    ModelFiles files{{root / "CMakeLists.txt", root / "src/main.cpp", foreign,
        outer / "CMakeLists.txt", outer / "src/main.cpp"}, {}};
    CHECK(hmi::engine::detail::select_default_model_path(executable, "asr.gguf", files) ==
        executable / "models/asr.gguf");
    CHECK(files.visited(root / "models/asr.gguf"));
    CHECK(!files.visited(foreign));
    CHECK(!files.visited(outer / "CMakeLists.txt"));
}

HMI_TEST(model_paths_keep_standalone_deployment_local)
{
    const std::filesystem::path executable = "C:/standalone/bin";
    const auto foreign = executable.parent_path() / "models/tts.gguf";
    ModelFiles files{{foreign}, {}};
    CHECK(hmi::engine::detail::select_default_model_path(executable, "tts.gguf", files) ==
        executable / "models/tts.gguf");
    CHECK(!files.visited(foreign));
}
