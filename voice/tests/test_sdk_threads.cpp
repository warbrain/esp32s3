#include "check.hpp"
#include "engine/sdk_threads.hpp"

#include <vector>

namespace
{
class FakeOpenMp
{
public:
    hmi::engine::detail::OpenMpThreadApi api()
    {
        return {
            [this] { ++reads; return maximum; },
            [this](int requested) { writes.push_back(requested); maximum = requested; }
        };
    }

    int maximum = 12;
    int reads = 0;
    std::vector<int> writes;
};
}

HMI_TEST(sdk_threads_sets_and_restores_the_calling_tasks_limit)
{
    FakeOpenMp runtime;
    {
        hmi::engine::detail::ScopedSdkThreads scope(4, runtime.api());
        CHECK(runtime.maximum == 4);
        CHECK(scope.maximum_threads() == 4);
        // Diagnostics must read the runtime, rather than echo the request.
        runtime.maximum = 3;
        CHECK(scope.maximum_threads() == 3);
    }
    CHECK(runtime.maximum == 12);
    CHECK(runtime.writes == std::vector<int>({4, 12}));
    CHECK(runtime.reads == 3);
}

HMI_TEST(sdk_threads_nested_scopes_restore_each_enclosing_limit)
{
    FakeOpenMp runtime;
    {
        hmi::engine::detail::ScopedSdkThreads outer(4, runtime.api());
        {
            hmi::engine::detail::ScopedSdkThreads inner(2, runtime.api());
            CHECK(inner.maximum_threads() == 2);
        }
        CHECK(outer.maximum_threads() == 4);
    }
    CHECK(runtime.maximum == 12);
    CHECK(runtime.writes == std::vector<int>({4, 2, 4, 12}));
}

HMI_TEST(sdk_threads_missing_runtime_functions_leave_the_limit_unchanged)
{
    FakeOpenMp runtime;
    auto api = runtime.api();
    api.set_num_threads = {};
    {
        hmi::engine::detail::ScopedSdkThreads scope(4, api);
        CHECK(scope.maximum_threads() == 12);
    }
    CHECK(runtime.writes.empty());
    CHECK(runtime.reads == 1);

    api = runtime.api();
    api.get_max_threads = {};
    {
        hmi::engine::detail::ScopedSdkThreads scope(4, api);
        CHECK(scope.maximum_threads() == 0);
    }
    CHECK(runtime.maximum == 12 && runtime.writes.empty());
    CHECK(runtime.reads == 1);

    hmi::engine::detail::ScopedSdkThreads unavailable(4, {});
    CHECK(unavailable.maximum_threads() == 0);
}

HMI_TEST(sdk_threads_nonpositive_requests_leave_the_limit_unchanged)
{
    FakeOpenMp runtime;
    {
        hmi::engine::detail::ScopedSdkThreads zero(0, runtime.api());
        hmi::engine::detail::ScopedSdkThreads negative(-1, runtime.api());
        CHECK(runtime.maximum == 12);
    }
    CHECK(runtime.writes.empty());
    CHECK(runtime.reads == 0);
}

HMI_TEST(sdk_threads_does_not_override_an_unreadable_prior_limit)
{
    FakeOpenMp runtime;
    runtime.maximum = 0;
    {
        hmi::engine::detail::ScopedSdkThreads scope(4, runtime.api());
        CHECK(scope.maximum_threads() == 0);
    }
    CHECK(runtime.writes.empty());
}
