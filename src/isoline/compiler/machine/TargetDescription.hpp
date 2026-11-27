// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace isoline::compiler::machine
{

/// An instruction set the compiler can describe. A row of the target table (P5 Task 16) exists for
/// each architecture that has a backend; describing one does not imply that it has.
enum class Architecture : uint8_t
{
    /// x86-64 (AMD64, Intel 64). Spelled X64, as Windows does: the naming rule's CamelCase has no
    /// room for x86_64's underscore.
    X64,
    Arm64, ///< AArch64 (ARMv8-A and later); its backend is P6's.
};

/// The operating-system ABI generated code runs under: which calling convention the host uses, and
/// which platform rules (hardening, unwinding) apply. On x86-64, Apple uses the SystemV convention but
/// is a row of its own, so that the platform tables and an artifact's key can tell macOS apart.
enum class OperatingSystemAbi : uint8_t
{
    SystemV, ///< Linux, the BSDs and other System V platforms.
    Windows, ///< Windows (the Microsoft x64 or arm64 convention).
    Apple,   ///< macOS and the other Apple platforms.
};

/// An optional instruction-set extension generated code may use, given by the embedder (never probed
/// by the compiler itself). P6 appends arm64's.
enum class CpuFeature : uint8_t
{
    Sse41,  ///< SSE4.1: roundss/roundsd, pinsr/pextr, pmulld.
    Sse42,  ///< SSE4.2.
    Popcnt, ///< POPCNT.
    Lzcnt,  ///< LZCNT (ABM).
    Bmi1,   ///< BMI1: TZCNT, ANDN.
    Bmi2,   ///< BMI2: SHLX, SHRX, SARX.
    Avx,    ///< AVX (VEX encodings).
    Avx2,   ///< AVX2.
};

/// The last CpuFeature enumerator.
inline constexpr auto LastCpuFeature = CpuFeature::Avx2;

/// How many CpuFeature enumerators there are.
inline constexpr auto CpuFeatureCount = static_cast<std::size_t>(LastCpuFeature) + 1;

/// A set of CPU features: one bit per CpuFeature.
class CpuFeatures
{
  public:
    /// The empty set: the architecture's baseline only.
    constexpr CpuFeatures() noexcept = default;

    /// This set and @p feature.
    /// @param feature The feature to add.
    /// @return The new set.
    [[nodiscard]] constexpr CpuFeatures with(CpuFeature feature) const noexcept
    {
        return CpuFeatures { _bits | bitOf(feature) };
    }

    /// Whether @p feature is in the set.
    /// @param feature The feature.
    /// @return Whether it is.
    [[nodiscard]] constexpr bool contains(CpuFeature feature) const noexcept
    {
        return (_bits & bitOf(feature)) != 0;
    }

    /// Whether every feature of @p other is in the set: whether code that needs @p other runs on a CPU
    /// of this set.
    /// @param other The features needed.
    /// @return Whether all of them are here.
    [[nodiscard]] constexpr bool containsAll(CpuFeatures other) const noexcept
    {
        return (_bits & other._bits) == other._bits;
    }

    /// Whether both sets hold the same features.
    [[nodiscard]] constexpr bool operator==(CpuFeatures const&) const noexcept = default;

  private:
    static_assert(CpuFeatureCount <= 32, "CpuFeatures stores one bit per CpuFeature in a uint32_t");

    constexpr explicit CpuFeatures(uint32_t bits) noexcept: _bits { bits } {}

    [[nodiscard]] static constexpr uint32_t bitOf(CpuFeature feature) noexcept
    {
        return uint32_t { 1 } << static_cast<uint32_t>(feature);
    }

    uint32_t _bits = 0;
};

/// What code is generated for: the instruction set, the operating-system ABI, and the CPU features it
/// may use. Data only, so that an artifact's key (P8) can name it without the backend.
struct TargetDescription
{
    Architecture architecture = Architecture::X64;        ///< The instruction set.
    OperatingSystemAbi abi = OperatingSystemAbi::SystemV; ///< The operating-system ABI.
    CpuFeatures features {};                              ///< The optional extensions generated code may use.

    /// Whether both describe the same target.
    [[nodiscard]] bool operator==(TargetDescription const&) const = default;
};

} // namespace isoline::compiler::machine
