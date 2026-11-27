// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>

namespace isoline::platform
{

/// Whether the processor (and the kernel) offer a capability.
enum class CapabilityState : uint8_t
{
    Unsupported, ///< It is absent, or this platform cannot tell.
    Supported,   ///< It is present and usable.
};

/// The hardware capabilities code generation and sealing depend on, probed once.
class IHardwareCapabilities
{
  public:
    virtual ~IHardwareCapabilities() = default;

    /// Whether arm64 branch-target identification (BTI) can be enforced on executable pages.
    /// @return Its state.
    [[nodiscard]] virtual CapabilityState branchTargetIdentification() const noexcept = 0;
};

/// Linux's auxiliary vector (getauxval): how the kernel tells a process its hardware capabilities.
class IAuxiliaryVector
{
  public:
    virtual ~IAuxiliaryVector() = default;

    /// The AT_HWCAP2 entry: the second word of hardware-capability bits.
    /// @return Its bits; 0 when the kernel passes none.
    [[nodiscard]] virtual uint64_t hardwareCapabilities2() const noexcept = 0;
};

/// AT_HWCAP2's BTI bit on Linux arm64 (HWCAP2_BTI, arch/arm64/include/uapi/asm/hwcap.h), spelled here
/// because only arm64 headers define it.
inline constexpr uint64_t LinuxArm64HardwareCapability2Bti = uint64_t { 1 } << 17;

/// Linux arm64's capabilities, as its auxiliary vector reports them, read once at construction.
class AuxiliaryVectorCapabilities final: public IHardwareCapabilities
{
  public:
    /// Reads the capabilities from @p auxiliaryVector.
    /// @param auxiliaryVector The process's auxiliary vector; not kept.
    explicit AuxiliaryVectorCapabilities(IAuxiliaryVector const& auxiliaryVector) noexcept;

    /// @copydoc IHardwareCapabilities::branchTargetIdentification
    [[nodiscard]] CapabilityState branchTargetIdentification() const noexcept override
    {
        return _branchTargetIdentification;
    }

  private:
    CapabilityState _branchTargetIdentification; ///< Whether AT_HWCAP2 has HWCAP2_BTI.
};

/// A platform with none of the capabilities: every one Unsupported.
class NoHardwareCapabilities final: public IHardwareCapabilities
{
  public:
    /// @copydoc IHardwareCapabilities::branchTargetIdentification
    [[nodiscard]] CapabilityState branchTargetIdentification() const noexcept override
    {
        return CapabilityState::Unsupported;
    }
};

/// The capabilities of the processor this process runs on, by this build's platform row
/// (the CMake-selected HardwareCapabilities{LinuxArm64,None}.cpp): Linux arm64 reads its auxiliary
/// vector, every other platform has none of them.
/// @return The capabilities, probed now.
[[nodiscard]] std::unique_ptr<IHardwareCapabilities> makeSystemHardwareCapabilities();

} // namespace isoline::platform
