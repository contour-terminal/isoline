// SPDX-License-Identifier: Apache-2.0
// The x86-64 row of the floating-point control register: MXCSR. CMake compiles it for x86-64 targets.
#include <xmmintrin.h>

#include <isoline/runtime/FloatEnvironment.hpp>

namespace isoline::runtime
{

namespace
{
    /// MXCSR's rounding control (bits 13-14; 00 is round-to-nearest-even).
    constexpr auto RoundingControl = FloatControlWord { 0b11U << 13U };
    /// MXCSR's flush-to-zero (bit 15) and denormals-are-zero (bit 6).
    constexpr auto Flushes = FloatControlWord { (1U << 15U) | (1U << 6U) };
    /// MXCSR's exception masks (bits 7-12), all set: no float operation traps.
    constexpr auto ExceptionMasks = FloatControlWord { 0b11'1111U << 7U };
    /// MXCSR's sticky exception flags (bits 0-5).
    constexpr auto ExceptionFlags = FloatControlWord { 0b11'1111U };

    constexpr auto Layout = FloatControlLayout {
        .cleared = RoundingControl | Flushes,
        .set = ExceptionMasks,
        .flushes = Flushes,
        .traps = ExceptionMasks,
        .flags = ExceptionFlags,
    };
} // namespace

FloatControlLayout const& hostFloatControlLayout() noexcept
{
    return Layout;
}

FloatControlWord readFloatControl() noexcept
{
    return _mm_getcsr();
}

void writeFloatControl(FloatControlWord word) noexcept
{
    _mm_setcsr(static_cast<unsigned int>(word));
}

} // namespace isoline::runtime
