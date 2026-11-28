// SPDX-License-Identifier: Apache-2.0
// CPUID through GCC's and Clang's <cpuid.h>.

#include <cpuid.h>

#include <isoline/execution/CpuFeatureProbe.hpp>

namespace isoline::execution
{

CpuidRegisters cpuid(uint32_t leaf, uint32_t subleaf) noexcept
{
    auto registers = CpuidRegisters {};
    __cpuid_count(leaf, subleaf, registers[0], registers[1], registers[2], registers[3]);
    return registers;
}

} // namespace isoline::execution
