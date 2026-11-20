#include "ggml_probe_policy.hpp"
#include <cstdio>

int main()
{
    using hmi::diagnostics::cpu_score_os_safe;
    int failed = 0;
    const auto check = [&failed](bool result) { if (!result) ++failed; };
    check(!cpu_score_os_safe(0, 0xe6));
    check(!cpu_score_os_safe(-1, 0xe6));
    check(cpu_score_os_safe(1, 0));
    check(cpu_score_os_safe(5, 0)); // baseline 1 + SSE42 weight 4
    check(!cpu_score_os_safe(17, 0)); // AVX hardware without OS state
    check(!cpu_score_os_safe(17, 2));
    check(cpu_score_os_safe(17, 6));
    check(!cpu_score_os_safe(44, 0)); // MSVC Haswell: baseline + FMA/F16C/BMI2/AVX2
    check(cpu_score_os_safe(44, 6));
    check(!cpu_score_os_safe(172, 6)); // AVX512 requires opmask and ZMM state too
    check(!cpu_score_os_safe(172, 0x26));
    check(cpu_score_os_safe(172, 0xe6));
    check(!cpu_score_os_safe(2049, 0xffff)); // AMX deliberately unsupported
    check(!cpu_score_os_safe(4097, 0xffff)); // unknown pinned score schema
    std::printf("ISA OS-state policy: 14 checks, %d failed\n", failed);
    return failed ? 1 : 0;
}
