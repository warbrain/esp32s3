#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace hmi::base
{
inline std::filesystem::path process_directory()
{
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, 32768);
    return count && count < 32768 ?
        std::filesystem::path(std::wstring(path, count)).parent_path() : std::filesystem::path{};
}

inline std::filesystem::path project_root(const std::filesystem::path &base)
{
    auto directory = base;
    std::error_code code;
    for (int depth = 0; depth < 8 && !directory.empty(); ++depth)
    {
        if (std::filesystem::is_regular_file(directory / "CMakeLists.txt", code) &&
            std::filesystem::is_regular_file(directory / "src/main.cpp", code)) return directory;
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return base;
}

// Resolve existing junctions, including the parent of a not-yet-created file.
// Relative paths belong to the application directory, independent of the CWD.
inline bool resolve_project_path(const std::filesystem::path &path,
                                 const std::filesystem::path &base,
                                 std::filesystem::path &resolved, std::string *error = nullptr)
{
    const auto fail = [error](const char *detail) {
        if (error) *error = detail;
        return false;
    };
    if (path.empty() || base.empty() || path.native().find(L'\0') != std::wstring::npos)
        return fail("project path must be non-empty and contain no NUL");
    std::error_code code;
    const auto root = std::filesystem::weakly_canonical(project_root(base), code);
    if (code || root.empty()) return fail("cannot resolve project root");
    const auto selected = std::filesystem::weakly_canonical(path.is_absolute() ? path : base / path, code);
    if (code) return fail("cannot resolve project path");
    auto part = selected.begin();
    for (const auto &root_part : root)
    {
        if (part == selected.end() || CompareStringOrdinal(root_part.c_str(), -1,
                part->c_str(), -1, TRUE) != CSTR_EQUAL)
            return fail("path must stay inside the project directory");
        ++part;
    }
    resolved = selected;
    if (error) error->clear();
    return true;
}
}
