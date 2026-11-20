#pragma once

#include <audiocpp.h>
#include "engine/native_diagnostics.hpp"

#include <string>

namespace hmi::engine::detail
{
struct SessionApi
{
    decltype(&::audiocpp_session_create) create = nullptr;
    decltype(&::audiocpp_session_free) free = nullptr;
    decltype(&::audiocpp_last_error) last_error = nullptr;
};

// "auto" is a host policy, never an option sent to the upstream SDK.
inline bool create_sdk_session(const SessionApi &api, audiocpp_model *model,
                               const char *task, const char *mode,
                               const std::string &backend, int threads,
                               audiocpp_session **out, std::string *error,
                               std::string *selected_backend = nullptr,
                               const audiocpp_options *cuda_tts_options = nullptr)
{
    *out = nullptr;
    if (selected_backend) selected_backend->clear();
    const char *automatic[] = {"cuda", "vulkan", "cpu"};
    const bool retry = backend == "auto";
    std::string failures;
    for (int attempt = 0; attempt < (retry ? 3 : 1); ++attempt)
    {
        const char *candidate = retry ? automatic[attempt] : backend.c_str();
        const audiocpp_backend_config config{candidate, 0, threads};
        audiocpp_session *session = nullptr;
        // CUDA-specific tuning must not leak into another task or backend on fallback.
        const auto *options = std::string(task) == "tts" && std::string(candidate) == "cuda" ?
            cuda_tts_options : nullptr;
        const auto status = api.create(model, task, mode, &config, options, &session);
        if (status == AUDIOCPP_OK && session)
        {
            *out = session;
            if (selected_backend) *selected_backend = candidate;
            if (error) error->clear();
            return true;
        }
        // Copy thread-local diagnostics before cleanup can replace them.
        const char *detail = api.last_error();
        const std::string message = status == AUDIOCPP_OK ? "engine returned no session" :
            (detail && *detail ? native_diagnostic(detail) : "engine status " + std::to_string(static_cast<int>(status)));
        if (session) api.free(session);
        if (!failures.empty()) failures += "; ";
        failures += std::string(candidate) + ": " + message;
    }
    if (error) *error = std::string("create ") + task + " " + mode + " session: " + failures;
    return false;
}
}
