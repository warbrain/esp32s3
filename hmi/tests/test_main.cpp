#include "check.hpp"
#include "environment.hpp"

#include <cstdio>

namespace hmi_test
{
namespace
{
int failures = 0;
}

std::vector<Entry> &registry()
{
    // Function-local static so the registry outlives static initialisation
    // order; this is test plumbing, not application state.
    static std::vector<Entry> entries;
    return entries;
}

void report_failure(const char *expression, const char *file, int line)
{
    ++failures;
    std::fprintf(stderr, "%s(%d): CHECK failed: %s\n", file, line, expression);
}

int run_all()
{
    for (const Entry &entry : registry())
    {
        const int before = failures;
        entry.fn();
        std::printf("[%s] %s\n", before == failures ? " ok " : "FAIL", entry.name);
    }
    std::printf("%d test(s), %d failure(s)\n", static_cast<int>(registry().size()), failures);
    return failures == 0 ? 0 : 1;
}
}

int main()
{
    if (!hmi_test::initialize_environment()) return 1;
    return hmi_test::run_all();
}
