#pragma once

#include <windows.h>

#include <functional>
#include <utility>

namespace hmi::engine::detail
{
struct OpenMpThreadApi
{
    std::function<int()> get_max_threads;
    std::function<void(int)> set_num_threads;
};

inline OpenMpThreadApi loaded_openmp_thread_api()
{
    // Use the runtime already loaded by native.dll; never load another copy.
    const auto module = GetModuleHandleW(L"vcomp140.dll");
    if (!module) return {};
    using GetMaxThreads = int (__cdecl *)();
    using SetNumThreads = void (__cdecl *)(int);
    const auto get = reinterpret_cast<GetMaxThreads>(GetProcAddress(module, "omp_get_max_threads"));
    const auto set = reinterpret_cast<SetNumThreads>(GetProcAddress(module, "omp_set_num_threads"));
    return {get, set};
}

// OpenMP's thread-count ICV belongs to the calling task. Keep the override on
// the SDK worker that performs DSP, and restore its prior limit on every exit.
class ScopedSdkThreads
{
public:
    explicit ScopedSdkThreads(int requested, OpenMpThreadApi api = loaded_openmp_thread_api())
        : api_(std::move(api))
    {
        if (requested <= 0 || !api_.get_max_threads || !api_.set_num_threads) return;
        previous_ = api_.get_max_threads();
        if (previous_ <= 0) return;
        api_.set_num_threads(requested);
        restore_ = true;
    }

    ~ScopedSdkThreads()
    {
        if (restore_) api_.set_num_threads(previous_);
    }

    ScopedSdkThreads(const ScopedSdkThreads &) = delete;
    ScopedSdkThreads &operator=(const ScopedSdkThreads &) = delete;
    ScopedSdkThreads(ScopedSdkThreads &&) = delete;
    ScopedSdkThreads &operator=(ScopedSdkThreads &&) = delete;

    // This is the current ICV limit, not an observed parallel-team size.
    int maximum_threads() const noexcept
    {
        return api_.get_max_threads ? api_.get_max_threads() : 0;
    }

private:
    OpenMpThreadApi api_;
    int previous_ = 0;
    bool restore_ = false;
};
}
