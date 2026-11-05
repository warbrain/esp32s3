#pragma once

#include <filesystem>

namespace hmi::engine::detail
{
// The predicate keeps path selection independent of the process and filesystem.
template <typename IsRegularFile>
std::filesystem::path select_default_model_path(const std::filesystem::path &executable_directory,
                                                const std::filesystem::path &filename,
                                                IsRegularFile &&is_regular_file)
{
    const auto local = executable_directory / "models" / filename;
    if (is_regular_file(local)) return local;

    auto directory = executable_directory;
    for (int depth = 0; depth < 8 && !directory.empty(); ++depth)
    {
        // Ancestors are inspected only for the nearest HMI project boundary.
        if (is_regular_file(directory / "CMakeLists.txt") &&
            is_regular_file(directory / "src" / "main.cpp"))
        {
            const auto project = directory / "models" / filename;
            return is_regular_file(project) ? project : local;
        }
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return local;
}
}
