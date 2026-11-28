// SPDX-License-Identifier: Apache-2.0
// CPUID through MSVC's (and clang-cl's) <intrin.h>.

#include <bit>

#include <intrin.h>

#include <isoline/execution/CpuFeatureProbe.hpp>

namespace isoline::execution
{

CpuidRegisters cpuid(uint32_t leaf, uint32_t subleaf) noexcept
{
    auto words = std::array<int, 4> {};
    __cpuidex(words.data(), static_cast<int>(leaf), static_cast<int>(subleaf));
    return std::bit_cast<CpuidRegisters>(words);
}

} // namespace isoline::execution
