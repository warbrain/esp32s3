#include "engine/model_location.hpp"
#include "engine/model_location_detail.hpp"

#include <windows.h>

#include <cstdlib>
#include <filesystem>

namespace hmi::engine
{
namespace
{
std::string utf8(const std::filesystem::path &path)
{
    const auto value = path.generic_u8string();
    return std::string(value.begin(), value.end());
}
}

std::string default_model_path(std::string_view filename)
{
    wchar_t *configured = nullptr;
    std::size_t size = 0;
    _wdupenv_s(&configured, &size, L"HMI_MODEL_DIR");
    const std::filesystem::path override = configured ? configured : L"";
    std::free(configured);
    const std::filesystem::path name{std::string(filename)};
    if (!override.empty()) return utf8(override / name);

    wchar_t module[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, module, 32768);
    const auto base = std::filesystem::path(std::wstring(module, count)).parent_path();
    return utf8(detail::select_default_model_path(base, name, [](const std::filesystem::path &path) {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error);
    }));
}
}
