// SPDX-License-Identifier: Apache-2.0
// The arm64 row of the floating-point control register: FPCR. CMake compiles it for arm64 targets
// built by GCC or Clang (Apple Clang included); MSVC on arm64 takes the portable row.
#include <isoline/runtime/FloatEnvironment.hpp>

namespace isoline::runtime
{

namespace
{
    /// FPCR's rounding mode, RMode (bits 22-23; 00 is round-to-nearest-even).
    constexpr auto RoundingMode = FloatControlWord { 0b11U << 22U };
    /// FPCR's flush-to-zero, FZ (bit 24), and its half-precision twin, FZ16 (bit 19).
    constexpr auto Flushes = FloatControlWord { (1U << 24U) | (1U << 19U) };
    /// FPCR's default-NaN mode, DN (bit 25).
    constexpr auto DefaultNan = FloatControlWord { 1U << 25U };
    /// FPCR's trap enables: IOE, DZE, OFE, UFE, IXE (bits 8-12) and IDE (bit 15).
    constexpr auto TrapEnables = FloatControlWord { (0b1'1111U << 8U) | (1U << 15U) };
    /// FPCR's alternate floating-point behaviour (FEAT_AFP): FIZ (bit 0) flushes inputs to zero, AH
    /// (bit 1) changes it further, and NEP (bit 2) changes what scalar operations leave in the rest
    /// of the vector register. All read as zero, and ignore writes of zero, on cores without it.
    constexpr auto AlternateBehaviour = FloatControlWord { 0b111U };

    constexpr auto Layout = FloatControlLayout {
        .cleared = RoundingMode | Flushes | DefaultNan | TrapEnables | AlternateBehaviour,
        .set = 0,
        .flushes = Flushes,
        .traps = TrapEnables,
    };
} // namespace

FloatControlLayout const& hostFloatControlLayout() noexcept
{
    return Layout;
}

FloatControlWord readFloatControl() noexcept
{
    auto word = FloatControlWord {};
    asm volatile("mrs %0, fpcr" : "=r"(word));
    return word;
}

void writeFloatControl(FloatControlWord word) noexcept
{
    asm volatile("msr fpcr, %0" : : "r"(word));
}

} // namespace isoline::runtime
