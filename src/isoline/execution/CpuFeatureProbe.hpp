// SPDX-License-Identifier: Apache-2.0
#pragma once

// The CPU the code runs on (P5 Decision "CPU features come from the embedder"): the embedder names the
// features its compiled code may use, and a load checks them against the running CPU through an injected
// ICpuFeatureProbe, so that code needing a feature the CPU lacks is refused rather than met with SIGILL. The
// platform's half of HostCpuFeatureProbe is CMake-selected by the target architecture
// (CpuFeatureProbeX86_64.cpp, CpuFeatureProbePortable.cpp), and on x86-64 CPUID's by the compiler's frontend
// (CpuidGnu.cpp, CpuidMsvc.cpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>

#include <isoline/compiler/machine/TargetDescription.hpp>

namespace isoline::execution
{

/// What the CPU code runs on provides: the DI seam for the running CPU.
class ICpuFeatureProbe
{
  public:
    virtual ~ICpuFeatureProbe() = default;

    /// The features of the CPU the calling thread runs on.
    [[nodiscard]] virtual compiler::machine::CpuFeatures features() const noexcept = 0;

  protected:
    ICpuFeatureProbe() = default;
    ICpuFeatureProbe(ICpuFeatureProbe const&) = default;
    ICpuFeatureProbe(ICpuFeatureProbe&&) = default;
    ICpuFeatureProbe& operator=(ICpuFeatureProbe const&) = default;
    ICpuFeatureProbe& operator=(ICpuFeatureProbe&&) = default;
};

/// The running CPU: CPUID on x86-64 (CpuidFeatureTable); the empty set elsewhere, where no selection row is
/// gated on a feature yet. Stateless.
class HostCpuFeatureProbe final: public ICpuFeatureProbe
{
  public:
    [[nodiscard]] compiler::machine::CpuFeatures features() const noexcept override;
};

/// One of the four registers CPUID returns.
enum class CpuidRegister : uint8_t
{
    Eax,
    Ebx,
    Ecx,
    Edx,
};

/// Where CPUID reports one CPU feature: a bit of a register of a leaf and subleaf.
struct CpuidFeatureRow
{
    compiler::machine::CpuFeature feature; ///< The feature.
    uint32_t leaf;                         ///< CPUID's leaf (EAX), basic or extended (0x8000'0000 and up).
    uint32_t subleaf;                      ///< Its subleaf (ECX).
    CpuidRegister reg;                     ///< The register holding the bit.
    uint8_t bit;                           ///< The bit.
};

/// Every feature HostCpuFeatureProbe reports on x86-64, from the Intel SDM's CPUID tables (and AMD's for
/// LZCNT, its ABM bit). AVX and AVX2 are not among them: using them needs the operating system's support too
/// (OSXSAVE and XGETBV), which arrives with the first selection row that needs either (P5b, SIMD).
inline constexpr auto CpuidFeatureTable = std::to_array<CpuidFeatureRow>({
    { compiler::machine::CpuFeature::Sse41, 1, 0, CpuidRegister::Ecx, 19 },
    { compiler::machine::CpuFeature::Sse42, 1, 0, CpuidRegister::Ecx, 20 },
    { compiler::machine::CpuFeature::Popcnt, 1, 0, CpuidRegister::Ecx, 23 },
    { compiler::machine::CpuFeature::Lzcnt, 0x8000'0001, 0, CpuidRegister::Ecx, 5 },
    { compiler::machine::CpuFeature::Bmi1, 7, 0, CpuidRegister::Ebx, 3 },
    { compiler::machine::CpuFeature::Bmi2, 7, 0, CpuidRegister::Ebx, 8 },
});

static_assert(
    [] {
        for (auto const index: std::views::iota(std::size_t { 1 }, CpuidFeatureTable.size()))
            if (CpuidFeatureTable[index - 1].feature >= CpuidFeatureTable[index].feature)
                return false;
        return true;
    }(),
    "CpuidFeatureTable must hold at most one row per CpuFeature, in enumerator order");

/// What CPUID returns for one leaf and subleaf, by CpuidRegister.
using CpuidRegisters = std::array<uint32_t, 4>;

/// Runs CPUID on the calling thread's CPU, through the compiler's intrinsic; all zeros on an architecture
/// without CPUID, where no leaf exists.
/// @param leaf EAX.
/// @param subleaf ECX.
/// @return EAX, EBX, ECX, EDX.
[[nodiscard]] CpuidRegisters cpuid(uint32_t leaf, uint32_t subleaf) noexcept;

/// The features @p read reports by CpuidFeatureTable: each row's bit, of a leaf the CPU has (leaf 0's and
/// 0x8000'0000's EAX name the highest basic and extended leaf).
/// @param read CPUID, or a test's stand-in.
/// @return The features.
template <typename Read>
[[nodiscard]] compiler::machine::CpuFeatures cpuidFeaturesOf(Read const& read) noexcept
{
    constexpr auto ExtendedLeaves = uint32_t { 0x8000'0000 };
    auto const highestBasic = read(0, 0)[0];
    auto const highestExtended = read(ExtendedLeaves, 0)[0];
    auto features = compiler::machine::CpuFeatures {};
    for (auto const& row: CpuidFeatureTable)
    {
        auto const highest = row.leaf >= ExtendedLeaves ? highestExtended : highestBasic;
        if (row.leaf > highest)
            continue;
        auto const word = read(row.leaf, row.subleaf)[static_cast<std::size_t>(row.reg)];
        if (((word >> row.bit) & 1U) != 0)
            features = features.with(row.feature);
    }
    return features;
}

} // namespace isoline::execution
