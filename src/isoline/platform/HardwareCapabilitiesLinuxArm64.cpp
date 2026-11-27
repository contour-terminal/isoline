// SPDX-License-Identifier: Apache-2.0
// Linux arm64's row: the capabilities its auxiliary vector reports (getauxval).
#include <sys/auxv.h>
#include <sys/mman.h>

#include <cstdint>
#include <memory>
#include <utility>

#include <asm/hwcap.h>
#include <isoline/platform/HardwareCapabilities.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>
#include <isoline/platform/VirtualMemory.hpp>

namespace isoline::platform
{

static_assert(ThisPlatform.branchTargets == BranchTargetProtectionKind::LinuxProtBti,
              "HardwareCapabilitiesLinuxArm64.cpp is Linux arm64's row");
static_assert(LinuxArm64HardwareCapability2Bti == HWCAP2_BTI, "the spelled-out HWCAP2_BTI is the kernel's");
static_assert(BranchTargetProtectionFlags[std::to_underlying(BranchTargetProtectionKind::LinuxProtBti)].flag
                  == PROT_BTI,
              "the spelled-out PROT_BTI is the kernel's");

namespace
{

    /// The process's own auxiliary vector.
    class SystemAuxiliaryVector final: public IAuxiliaryVector
    {
      public:
        [[nodiscard]] uint64_t hardwareCapabilities2() const noexcept override
        {
            return getauxval(AT_HWCAP2);
        }
    };

} // namespace

std::unique_ptr<IHardwareCapabilities> makeSystemHardwareCapabilities()
{
    return std::make_unique<AuxiliaryVectorCapabilities>(SystemAuxiliaryVector {});
}

} // namespace isoline::platform
