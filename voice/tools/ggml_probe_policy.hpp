#pragma once
#include <cstdint>

namespace hmi::diagnostics
{
// The bundled cpu-feats.cpp uses score = 1 + unique feature weights.
// Its CPUID checks omit OS context support, so guard XCR0 before init.
constexpr bool cpu_score_os_safe(int score, std::uint64_t xstate) noexcept
{
    if (score <= 0) return false;
    const auto features = static_cast<std::uint32_t>(score - 1);
    if ((features & ~0xfffu) != 0 || (features & 0x800u) != 0) return false;
    if ((features & 0x780u) != 0) return (xstate & 0xe6u) == 0xe6u;
    if ((features & 0x73u) != 0) return (xstate & 6u) == 6u;
    return true;
}
}
