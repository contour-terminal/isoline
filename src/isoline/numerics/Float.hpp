// SPDX-License-Identifier: Apache-2.0
#pragma once

// The typed float kernels of WebAssembly's numeric instructions (spec 4.3.3) and the conversions to
// and from floats (spec 4.3.4): one template per operator over the operand's value type, with no
// boxed runtime::Value and no dispatch. A NaN result of arithmetic is the positive canonical NaN, so
// every executor that evaluates float instructions through them produces the same bits.
//
// Host arithmetic (add, sub, mul, div, sqrt) must be one correctly rounded operation each: the
// targets that include this header compile with floating-point contraction off (a row of
// cmake/IsolinePedantic.cmake), so that no multiply and add are fused into one rounding.

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <ranges>
#include <type_traits>

#include <isoline/numerics/Integer.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

// The float arithmetic below relies on the host evaluating float and double in their own precision,
// IEEE 754 binary32 and binary64, so that a single operation is a single correctly rounded one.
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559,
              "the float kernels need IEEE 754 float and double");
static_assert(FLT_EVAL_METHOD == 0, "the float kernels need floats evaluated in their own precision");

namespace isoline::numerics
{

/// The canonical NaN of f32 (spec 4.3.3): quiet, with no other payload bit; positive.
inline constexpr auto CanonicalNanF32 = uint32_t { 0x7FC0'0000 };
/// The canonical NaN of f64: quiet, with no other payload bit; positive.
inline constexpr auto CanonicalNanF64 = uint64_t { 0x7FF8'0000'0000'0000 };

// --- Float formats ------------------------------------------------------------------------

/// The IEEE 754 layout of a float value type: F32 (binary32) or F64 (binary64).
template <typename Box>
struct FloatFormat;

/// f32: IEEE 754 binary32.
template <>
struct FloatFormat<runtime::F32>
{
    using Bits = uint32_t;                                ///< The bits.
    using Float = float;                                  ///< The host type of the same layout.
    static constexpr int MantissaBits = 23;               ///< The stored mantissa's width.
    static constexpr int ExponentBias = 127;              ///< The exponent field's bias.
    static constexpr Bits CanonicalNan = CanonicalNanF32; ///< The canonical NaN.
};

/// f64: IEEE 754 binary64.
template <>
struct FloatFormat<runtime::F64>
{
    using Bits = uint64_t;                                ///< The bits.
    using Float = double;                                 ///< The host type of the same layout.
    static constexpr int MantissaBits = 52;               ///< The stored mantissa's width.
    static constexpr int ExponentBias = 1023;             ///< The exponent field's bias.
    static constexpr Bits CanonicalNan = CanonicalNanF64; ///< The canonical NaN.
};

/// The unsigned type holding the bits of a float value type.
template <typename Box>
using FloatBits = FloatFormat<Box>::Bits;

/// The host float type of a float value type's layout.
template <typename Box>
using FloatOf = FloatFormat<Box>::Float;

/// The sign bit.
template <typename Box>
inline constexpr auto SignMask =
    static_cast<FloatBits<Box>>(FloatBits<Box> { 1 } << (std::numeric_limits<FloatBits<Box>>::digits - 1));

/// The stored mantissa's bits.
template <typename Box>
inline constexpr auto MantissaMask =
    static_cast<FloatBits<Box>>((FloatBits<Box> { 1 } << FloatFormat<Box>::MantissaBits) - 1);

/// The bits of +infinity: the exponent field all ones, the mantissa zero.
template <typename Box>
inline constexpr auto InfinityBits = static_cast<FloatBits<Box>>(~SignMask<Box> & ~MantissaMask<Box>);

/// The bits of 1.0, the biased exponent of zero alone.
template <typename Box>
inline constexpr auto OneBits = static_cast<FloatBits<Box>>(FloatBits<Box> { FloatFormat<Box>::ExponentBias }
                                                            << FloatFormat<Box>::MantissaBits);

/// The bits of 0.5.
template <typename Box>
inline constexpr auto HalfBits = static_cast<FloatBits<Box>>(
    FloatBits<Box> { FloatFormat<Box>::ExponentBias - 1 } << FloatFormat<Box>::MantissaBits);

/// The bits of @p value without its sign.
template <typename Box>
[[nodiscard]] constexpr FloatBits<Box> magnitudeOf(Box value) noexcept
{
    return static_cast<FloatBits<Box>>(value.bits & ~SignMask<Box>);
}

/// Whether @p value is a NaN, of any sign and payload.
template <typename Box>
[[nodiscard]] constexpr bool isNan(Box value) noexcept
{
    return magnitudeOf(value) > InfinityBits<Box>;
}

/// The value as a host float; for arithmetic only, as loading a NaN may change its bits.
template <typename Box>
[[nodiscard]] constexpr FloatOf<Box> toHost(Box value) noexcept
{
    return std::bit_cast<FloatOf<Box>>(value.bits);
}

/// The result of a host arithmetic operation: its bits, or the canonical NaN for any NaN.
template <typename Box>
[[nodiscard]] Box arithmeticResult(FloatOf<Box> result) noexcept
{
    if (std::isnan(result))
        return Box { FloatFormat<Box>::CanonicalNan };
    return Box { std::bit_cast<FloatBits<Box>>(result) };
}

// --- Float operators (spec 4.3.3) ---------------------------------------------------------

/// abs: the value with its sign cleared, on bits, so that a NaN keeps its payload.
template <typename Box>
[[nodiscard]] constexpr Box fabs(Box value) noexcept
{
    return Box { magnitudeOf(value) };
}

/// neg: the value with its sign flipped, on bits, so that a NaN keeps its payload.
template <typename Box>
[[nodiscard]] constexpr Box fneg(Box value) noexcept
{
    return Box { static_cast<FloatBits<Box>>(value.bits ^ SignMask<Box>) };
}

/// copysign: the magnitude of @p lhs with the sign of @p rhs, on bits.
template <typename Box>
[[nodiscard]] constexpr Box copysign(Box lhs, Box rhs) noexcept
{
    return Box { static_cast<FloatBits<Box>>(magnitudeOf(lhs) | (rhs.bits & SignMask<Box>) ) };
}

/// Which way ceil, floor, trunc and nearest round a value that is not integral.
enum class Rounding : uint8_t
{
    TowardZero,  ///< trunc: toward zero.
    Down,        ///< floor: toward negative infinity.
    Up,          ///< ceil: toward positive infinity.
    NearestEven, ///< nearest: to the nearer integral value, ties to the even one.
};

/// Rounds @p value to an integral value, on its bits alone.
///
/// A value whose exponent places the units digit at or beyond the mantissa's last bit is
/// integral already (infinities included); one of magnitude below 1 rounds to a signed 0 or 1;
/// otherwise the bits below the units digit are cleared, and one unit added to the magnitude
/// when rounding away from zero, the carry running into the exponent as it must.
template <typename Box>
[[nodiscard]] constexpr Box roundToIntegral(Box value, Rounding rounding) noexcept
{
    using Bits = FloatBits<Box>;
    if (isNan(value))
        return Box { FloatFormat<Box>::CanonicalNan };
    auto const sign = static_cast<Bits>(value.bits & SignMask<Box>);
    auto const magnitude = magnitudeOf(value);
    auto const isNegative = sign != 0;
    auto const exponent =
        static_cast<int>(magnitude >> FloatFormat<Box>::MantissaBits) - FloatFormat<Box>::ExponentBias;
    if (exponent >= FloatFormat<Box>::MantissaBits || magnitude == 0)
        return value;

    if (exponent < 0)
    {
        auto const awayFromZero = [&] {
            switch (rounding)
            {
                case Rounding::TowardZero: return false;
                case Rounding::Down: return isNegative;
                case Rounding::Up: return !isNegative;
                case Rounding::NearestEven: break; // decided below the switch
            }
            return magnitude > HalfBits<Box>;
        }();
        return Box { static_cast<Bits>(sign | (awayFromZero ? OneBits<Box> : Bits { 0 })) };
    }

    auto const fraction = static_cast<Bits>(MantissaMask<Box> >> exponent);
    auto const remainder = static_cast<Bits>(magnitude & fraction);
    if (remainder == 0)
        return value;
    auto const truncated = static_cast<Bits>(magnitude & ~fraction);
    auto const unit = static_cast<Bits>(fraction + 1);
    auto const awayFromZero = [&] {
        switch (rounding)
        {
            case Rounding::TowardZero: return false;
            case Rounding::Down: return isNegative;
            case Rounding::Up: return !isNegative;
            case Rounding::NearestEven: break; // decided below the switch
        }
        auto const half = static_cast<Bits>(unit >> 1);
        // The unit bit is the integral part's lowest bit; for exponent 0 it is the exponent's
        // lowest bit, which is set, as 1 is odd.
        return remainder > half || (remainder == half && (truncated & unit) != 0);
    }();
    return Box { static_cast<Bits>(sign | (awayFromZero ? truncated + unit : truncated)) };
}

/// ceil: rounded up to an integral value.
template <typename Box>
[[nodiscard]] constexpr Box ceil(Box value) noexcept
{
    return roundToIntegral(value, Rounding::Up);
}

/// floor: rounded down to an integral value.
template <typename Box>
[[nodiscard]] constexpr Box floor(Box value) noexcept
{
    return roundToIntegral(value, Rounding::Down);
}

/// trunc: rounded toward zero to an integral value.
template <typename Box>
[[nodiscard]] constexpr Box trunc(Box value) noexcept
{
    return roundToIntegral(value, Rounding::TowardZero);
}

/// nearest: rounded to the nearest integral value, ties to even.
template <typename Box>
[[nodiscard]] constexpr Box nearest(Box value) noexcept
{
    return roundToIntegral(value, Rounding::NearestEven);
}

/// sqrt: the host's correctly rounded square root; a NaN result is the canonical NaN.
template <typename Box>
[[nodiscard]] Box sqrt(Box value) noexcept
{
    return arithmeticResult<Box>(std::sqrt(toHost(value)));
}

/// add: the host's correctly rounded sum; a NaN result is the canonical NaN.
template <typename Box>
[[nodiscard]] Box fadd(Box lhs, Box rhs) noexcept
{
    return arithmeticResult<Box>(toHost(lhs) + toHost(rhs));
}

/// sub: the host's correctly rounded difference; a NaN result is the canonical NaN.
template <typename Box>
[[nodiscard]] Box fsub(Box lhs, Box rhs) noexcept
{
    return arithmeticResult<Box>(toHost(lhs) - toHost(rhs));
}

/// mul: the host's correctly rounded product; a NaN result is the canonical NaN.
template <typename Box>
[[nodiscard]] Box fmul(Box lhs, Box rhs) noexcept
{
    return arithmeticResult<Box>(toHost(lhs) * toHost(rhs));
}

/// div: the host's correctly rounded quotient; a NaN result is the canonical NaN.
template <typename Box>
[[nodiscard]] Box fdiv(Box lhs, Box rhs) noexcept
{
    return arithmeticResult<Box>(toHost(lhs) / toHost(rhs));
}

/// Whether @p value is +0 or -0.
[[nodiscard]] constexpr bool isZero(auto value) noexcept
{
    return magnitudeOf(value) == 0;
}

/// min: a NaN operand gives the canonical NaN, and -0 is below +0.
template <typename Box>
[[nodiscard]] constexpr Box fmin(Box lhs, Box rhs) noexcept
{
    if (isNan(lhs) || isNan(rhs))
        return Box { FloatFormat<Box>::CanonicalNan };
    if (isZero(lhs) && isZero(rhs))
        return Box { static_cast<FloatBits<Box>>(lhs.bits | rhs.bits) };
    return toHost(lhs) < toHost(rhs) ? lhs : rhs;
}

/// max: a NaN operand gives the canonical NaN, and +0 is above -0.
template <typename Box>
[[nodiscard]] constexpr Box fmax(Box lhs, Box rhs) noexcept
{
    if (isNan(lhs) || isNan(rhs))
        return Box { FloatFormat<Box>::CanonicalNan };
    if (isZero(lhs) && isZero(rhs))
        return Box { static_cast<FloatBits<Box>>(lhs.bits & rhs.bits) };
    return toHost(lhs) > toHost(rhs) ? lhs : rhs;
}

/// eq: false when either operand is a NaN; -0 equals +0.
template <typename Box>
[[nodiscard]] constexpr bool feq(Box lhs, Box rhs) noexcept
{
    return toHost(lhs) == toHost(rhs);
}

/// ne: true when either operand is a NaN.
template <typename Box>
[[nodiscard]] constexpr bool fne(Box lhs, Box rhs) noexcept
{
    return toHost(lhs) != toHost(rhs);
}

/// lt: false when either operand is a NaN.
template <typename Box>
[[nodiscard]] constexpr bool flt(Box lhs, Box rhs) noexcept
{
    return toHost(lhs) < toHost(rhs);
}

/// gt: false when either operand is a NaN.
template <typename Box>
[[nodiscard]] constexpr bool fgt(Box lhs, Box rhs) noexcept
{
    return toHost(lhs) > toHost(rhs);
}

/// le: false when either operand is a NaN.
template <typename Box>
[[nodiscard]] constexpr bool fle(Box lhs, Box rhs) noexcept
{
    return toHost(lhs) <= toHost(rhs);
}

/// ge: false when either operand is a NaN.
template <typename Box>
[[nodiscard]] constexpr bool fge(Box lhs, Box rhs) noexcept
{
    return toHost(lhs) >= toHost(rhs);
}

// --- Conversions to and from floats (spec 4.3.4) ------------------------------------------

/// 2 to the power of @p exponent, exactly.
[[nodiscard]] consteval double powerOfTwo(int exponent) noexcept
{
    auto result = 1.0;
    for ([[maybe_unused]] auto const _: std::views::iota(0, exponent))
        result *= 2.0;
    return result;
}

/// The range of @p Int, as the exact doubles min and max + 1: every bound is 0 or a power of 2.
template <typename Int>
struct IntRange
{
    static constexpr double Min = std::is_signed_v<Int> ? -powerOfTwo(std::numeric_limits<Int>::digits) : 0.0;
    static constexpr double MaxPlusOne = powerOfTwo(std::numeric_limits<Int>::digits);
};

/// A non-NaN float as a host double; exact, as binary64 holds every binary32 value.
template <typename Box>
[[nodiscard]] constexpr double toDouble(Box value) noexcept
{
    return static_cast<double>(toHost(value));
}

/// trunc_s and trunc_u: a NaN traps as an invalid conversion, a value whose integral part
/// @p Int cannot hold as an overflow.
template <typename To, typename Int, typename From>
[[nodiscard]] std::expected<To, runtime::Trap> truncTrapping(From value) noexcept
{
    if (isNan(value))
        return runtime::trap(runtime::TrapCode::InvalidConversionToInteger);
    auto const integral = std::trunc(toDouble(value));
    if (integral < IntRange<Int>::Min || integral >= IntRange<Int>::MaxPlusOne)
        return runtime::trap(runtime::TrapCode::IntegerOverflow);
    return To { static_cast<BitsOf<To>>(static_cast<Int>(integral)) };
}

/// trunc_sat_s and trunc_sat_u: a NaN gives 0, a value out of range the nearest bound.
template <typename To, typename Int, typename From>
[[nodiscard]] To truncSaturating(From value) noexcept
{
    if (isNan(value))
        return To {};
    auto const integral = std::trunc(toDouble(value));
    if (integral < IntRange<Int>::Min)
        return To { static_cast<BitsOf<To>>(std::numeric_limits<Int>::min()) };
    if (integral >= IntRange<Int>::MaxPlusOne)
        return To { static_cast<BitsOf<To>>(std::numeric_limits<Int>::max()) };
    return To { static_cast<BitsOf<To>>(static_cast<Int>(integral)) };
}

/// The float nearest to sign · @p magnitude, ties to even, assembled on bits so that no host
/// conversion (some round twice for unsigned 64-bit sources) decides the result.
template <typename Box>
[[nodiscard]] constexpr Box fromMagnitude(uint64_t magnitude, FloatBits<Box> sign) noexcept
{
    using Bits = FloatBits<Box>;
    constexpr auto Precision = FloatFormat<Box>::MantissaBits + 1;
    if (magnitude == 0)
        return Box { sign };
    auto const width = std::bit_width(magnitude);
    auto exponent = width - 1;
    auto significand = magnitude;
    if (width <= Precision)
        significand <<= Precision - width;
    else
    {
        auto const shift = width - Precision;
        significand = magnitude >> shift;
        auto const rest = magnitude & ((uint64_t { 1 } << shift) - 1);
        auto const half = uint64_t { 1 } << (shift - 1);
        if (rest > half || (rest == half && (significand & 1) != 0))
            ++significand;
        if (significand == uint64_t { 1 } << Precision)
        {
            significand >>= 1;
            ++exponent;
        }
    }
    auto const biasedExponent = exponent + FloatFormat<Box>::ExponentBias;
    auto const biased = static_cast<Bits>(biasedExponent);
    return Box { static_cast<Bits>(sign | static_cast<Bits>(biased << FloatFormat<Box>::MantissaBits)
                                   | (static_cast<Bits>(significand) & MantissaMask<Box>) ) };
}

/// convert_s and convert_u: the integer @p value, read as @p Int, rounded to nearest.
template <typename To, typename Int, typename From>
[[nodiscard]] constexpr To convert(From value) noexcept
{
    auto const integer = static_cast<Int>(value.bits);
    if constexpr (std::is_signed_v<Int>)
    {
        if (integer < 0)
            return fromMagnitude<To>(uint64_t { 0 } - static_cast<uint64_t>(integer), SignMask<To>);
    }
    return fromMagnitude<To>(static_cast<uint64_t>(integer), 0);
}

/// f32.demote_f64: the nearest f32; a NaN gives the canonical NaN.
[[nodiscard]] inline runtime::F32 demote(runtime::F64 value) noexcept
{
    if (isNan(value))
        return runtime::F32 { CanonicalNanF32 };
    return runtime::F32 { std::bit_cast<uint32_t>(static_cast<float>(toHost(value))) };
}

/// f64.promote_f32: the same value, exactly; a NaN gives the canonical NaN.
[[nodiscard]] inline runtime::F64 promote(runtime::F32 value) noexcept
{
    if (isNan(value))
        return runtime::F64 { CanonicalNanF64 };
    return runtime::F64 { std::bit_cast<uint64_t>(toDouble(value)) };
}

/// reinterpret: the same bits, as the other type of their width.
template <typename To, typename From>
[[nodiscard]] constexpr To reinterpret(From value) noexcept
{
    static_assert(sizeof(To) == sizeof(From));
    return To { value.bits };
}

} // namespace isoline::numerics
