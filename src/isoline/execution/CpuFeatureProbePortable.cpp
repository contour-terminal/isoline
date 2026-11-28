// SPDX-License-Identifier: Apache-2.0
// HostCpuFeatureProbe where no selection row is gated on a feature yet: the empty set, the architecture's
// baseline, and no CPUID. P6 brings arm64's row.

#include <isoline/execution/CpuFeatureProbe.hpp>

namespace isoline::execution
{

CpuidRegisters cpuid(uint32_t /*leaf*/, uint32_t /*subleaf*/) noexcept
{
    return {};
}

compiler::machine::CpuFeatures HostCpuFeatureProbe::features() const noexcept
{
    return {};
}

} // namespace isoline::execution
