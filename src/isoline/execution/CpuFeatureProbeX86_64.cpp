// SPDX-License-Identifier: Apache-2.0
// HostCpuFeatureProbe on x86-64: CPUID, read by CpuidFeatureTable.

#include <isoline/execution/CpuFeatureProbe.hpp>

namespace isoline::execution
{

compiler::machine::CpuFeatures HostCpuFeatureProbe::features() const noexcept
{
    return cpuidFeaturesOf([](uint32_t leaf, uint32_t subleaf) noexcept { return cpuid(leaf, subleaf); });
}

} // namespace isoline::execution
