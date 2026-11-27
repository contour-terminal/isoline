// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <isoline/platform/HardwareCapabilities.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>

using namespace isoline::platform;

namespace
{

/// An auxiliary vector with a chosen AT_HWCAP2.
class FakeAuxiliaryVector final: public IAuxiliaryVector
{
  public:
    explicit FakeAuxiliaryVector(uint64_t hardwareCapabilities2) noexcept:
        _hardwareCapabilities2 { hardwareCapabilities2 }
    {
    }

    [[nodiscard]] uint64_t hardwareCapabilities2() const noexcept override { return _hardwareCapabilities2; }

  private:
    uint64_t _hardwareCapabilities2;
};

} // namespace

TEST_CASE("HardwareCapabilities.rowsPerPlatform")
{
    // Linux arm64 reads HWCAP2_BTI, and only that bit, from its auxiliary vector.
    CHECK(AuxiliaryVectorCapabilities { FakeAuxiliaryVector { LinuxArm64HardwareCapability2Bti } }
              .branchTargetIdentification()
          == CapabilityState::Supported);
    CHECK(AuxiliaryVectorCapabilities { FakeAuxiliaryVector { ~LinuxArm64HardwareCapability2Bti } }
              .branchTargetIdentification()
          == CapabilityState::Unsupported);
    CHECK(AuxiliaryVectorCapabilities { FakeAuxiliaryVector { 0 } }.branchTargetIdentification()
          == CapabilityState::Unsupported);
    // Every other row has none.
    CHECK(NoHardwareCapabilities {}.branchTargetIdentification() == CapabilityState::Unsupported);
    // This build's: a platform without a branch-target row never reports the capability.
    auto const system = makeSystemHardwareCapabilities();
    REQUIRE(system != nullptr);
    if (ThisPlatform.branchTargets == BranchTargetProtectionKind::None)
        CHECK(system->branchTargetIdentification() == CapabilityState::Unsupported);
}
