// SPDX-License-Identifier: Apache-2.0
// The row of every platform whose code generation probes no hardware capability.
#include <memory>

#include <isoline/platform/HardwareCapabilities.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>

namespace isoline::platform
{

static_assert(ThisPlatform.branchTargets == BranchTargetProtectionKind::None,
              "HardwareCapabilitiesNone.cpp is the row of platforms without a branch-target probe");

std::unique_ptr<IHardwareCapabilities> makeSystemHardwareCapabilities()
{
    return std::make_unique<NoHardwareCapabilities>();
}

} // namespace isoline::platform
