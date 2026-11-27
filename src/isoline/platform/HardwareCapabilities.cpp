// SPDX-License-Identifier: Apache-2.0
#include <isoline/platform/HardwareCapabilities.hpp>

namespace isoline::platform
{

AuxiliaryVectorCapabilities::AuxiliaryVectorCapabilities(IAuxiliaryVector const& auxiliaryVector) noexcept:
    _branchTargetIdentification { (auxiliaryVector.hardwareCapabilities2() & LinuxArm64HardwareCapability2Bti)
                                          != 0
                                      ? CapabilityState::Supported
                                      : CapabilityState::Unsupported }
{
}

} // namespace isoline::platform
