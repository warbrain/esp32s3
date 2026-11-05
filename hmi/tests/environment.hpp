#pragma once

#include "base/project_paths.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace hmi_test
{
// Change only this test process, including temporary files of child processes.
inline bool initialize_environment()
{
    const auto base = hmi::base::process_directory();
    std::filesystem::path directory;
    std::string error;
    if (!hmi::base::resolve_project_path(HMI_TEST_TMP_DIR, base, directory, &error))
    {
        std::fprintf(stderr, "Test temporary path rejected: %s\n", error.c_str());
        return false;
    }
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    if (code)
    {
        std::fprintf(stderr, "Cannot create test temporary directory: %s\n", code.message().c_str());
        return false;
    }
    for (const wchar_t *name : {L"TEMP", L"TMP"})
    {
        if (_wputenv_s(name, directory.c_str()) != 0 || !SetEnvironmentVariableW(name, directory.c_str()))
        {
            std::fprintf(stderr, "Cannot set process-local test temporary directory\n");
            return false;
        }
    }
    const auto selected = std::filesystem::temp_directory_path(code);
    std::filesystem::path verified;
    if (code || !hmi::base::resolve_project_path(selected, base, verified, &error))
    {
        std::fprintf(stderr, "Test temporary directory is unavailable or outside the project\n");
        return false;
    }
    return true;
}
}
