#pragma once

#include <vector>

namespace hmi_test
{
using TestFn = void (*)();

struct Entry
{
    const char *name;
    TestFn fn;
};

std::vector<Entry> &registry();
void report_failure(const char *expression, const char *file, int line);
int run_all();

struct Registrar
{
    Registrar(const char *name, TestFn fn) { registry().push_back({name, fn}); }
};
}

// HMI_TEST(name) { ... }
#define HMI_TEST(name)                                                 \
    static void name();                                                \
    static const ::hmi_test::Registrar name##_registrar(#name, name);  \
    static void name()

#define CHECK(expression)                                                \
    do                                                                   \
    {                                                                    \
        if (!(expression))                                               \
            ::hmi_test::report_failure(#expression, __FILE__, __LINE__); \
    } while (false)
