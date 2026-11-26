// SPDX-License-Identifier: Apache-2.0
#include <isoline/interp/Numerics.hpp>

#include <core/Assert.hpp>

#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <ranges>
#include <type_traits>
#include <variant>

#include <isoline/binary/Opcode.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

// The float arithmetic below relies on the host evaluating float and double in their own precision,
// IEEE 754 binary32 and binary64, so that a single operation is a single correctly rounded one.
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559,
              "the reference interpreter needs IEEE 754 float and double");
static_assert(FLT_EVAL_METHOD == 0,
              "the reference interpreter needs floats evaluated in their own precision");

namespace isoline::interp
{

namespace
{
    using binary::Opcode;
    using runtime::F32;
    using runtime::F64;
    using runtime::I32;
    using runtime::I64;
    using runtime::Trap;
    using runtime::trap;
    using runtime::TrapCode;
    using runtime::Value;

    /// The outcome of a numeric operator: a value, or the trap it ends with.
    using Result = std::expected<Value, Trap>;

    // --- Integer formats ----------------------------------------------------------------------

    /// The representation of an integer value type: I32 or I64.
    template <typename Box>
    struct IntFormat;

    template <>
    struct IntFormat<I32>
    {
        using Bits = uint32_t;
        using Signed = int32_t;
    };

    template <>
    struct IntFormat<I64>
    {
        using Bits = uint64_t;
        using Signed = int64_t;
    };

    template <typename Box>
    using BitsOf = IntFormat<Box>::Bits;

    template <typename Box>
    using SignedOf = IntFormat<Box>::Signed;

    /// The bit width of an integer value type.
    template <typename Box>
    inline constexpr auto WidthOf = static_cast<BitsOf<Box>>(std::numeric_limits<BitsOf<Box>>::digits);

    template <typename Box>
    [[nodiscard]] constexpr SignedOf<Box> asSigned(Box value) noexcept
    {
        return static_cast<SignedOf<Box>>(value.bits);
    }

    template <typename Box>
    [[nodiscard]] constexpr Box fromSigned(SignedOf<Box> value) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(value) };
    }

    // --- Integer operators (spec 4.3.2) -------------------------------------------------------

    template <typename Box>
    [[nodiscard]] constexpr Box clz(Box value) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(std::countl_zero(value.bits)) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box ctz(Box value) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(std::countr_zero(value.bits)) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box popcnt(Box value) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(std::popcount(value.bits)) };
    }

    /// Sign-extends the low bits of @p value that @p Narrow holds.
    template <typename Box, typename Narrow>
    [[nodiscard]] constexpr Box extendSigned(Box value) noexcept
    {
        return fromSigned<Box>(static_cast<Narrow>(value.bits));
    }

    template <typename Box>
    [[nodiscard]] constexpr Box add(Box lhs, Box rhs) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(lhs.bits + rhs.bits) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box sub(Box lhs, Box rhs) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(lhs.bits - rhs.bits) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box mul(Box lhs, Box rhs) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(lhs.bits * rhs.bits) };
    }

    template <typename Box>
    [[nodiscard]] constexpr std::expected<Box, Trap> divS(Box lhs, Box rhs) noexcept
    {
        if (rhs.bits == 0)
            return trap(TrapCode::IntegerDivideByZero);
        if (asSigned(lhs) == std::numeric_limits<SignedOf<Box>>::min() && asSigned(rhs) == -1)
            return trap(TrapCode::IntegerOverflow);
        return fromSigned<Box>(asSigned(lhs) / asSigned(rhs));
    }

    template <typename Box>
    [[nodiscard]] constexpr std::expected<Box, Trap> divU(Box lhs, Box rhs) noexcept
    {
        if (rhs.bits == 0)
            return trap(TrapCode::IntegerDivideByZero);
        return Box { static_cast<BitsOf<Box>>(lhs.bits / rhs.bits) };
    }

    /// The remainder, signed as the dividend; min rem -1 is 0, where the host would overflow.
    template <typename Box>
    [[nodiscard]] constexpr std::expected<Box, Trap> remS(Box lhs, Box rhs) noexcept
    {
        if (rhs.bits == 0)
            return trap(TrapCode::IntegerDivideByZero);
        if (asSigned(rhs) == -1)
            return Box {};
        return fromSigned<Box>(asSigned(lhs) % asSigned(rhs));
    }

    template <typename Box>
    [[nodiscard]] constexpr std::expected<Box, Trap> remU(Box lhs, Box rhs) noexcept
    {
        if (rhs.bits == 0)
            return trap(TrapCode::IntegerDivideByZero);
        return Box { static_cast<BitsOf<Box>>(lhs.bits % rhs.bits) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box bitAnd(Box lhs, Box rhs) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(lhs.bits & rhs.bits) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box bitOr(Box lhs, Box rhs) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(lhs.bits | rhs.bits) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box bitXor(Box lhs, Box rhs) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(lhs.bits ^ rhs.bits) };
    }

    /// The shift count: the operand modulo the bit width.
    template <typename Box>
    [[nodiscard]] constexpr int shiftCount(Box count) noexcept
    {
        return static_cast<int>(count.bits % WidthOf<Box>);
    }

    template <typename Box>
    [[nodiscard]] constexpr Box shl(Box lhs, Box rhs) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(lhs.bits << shiftCount(rhs)) };
    }

    /// The arithmetic right shift: C++20 defines >> of a negative value as one.
    template <typename Box>
    [[nodiscard]] constexpr Box shrS(Box lhs, Box rhs) noexcept
    {
        return fromSigned<Box>(static_cast<SignedOf<Box>>(asSigned(lhs) >> shiftCount(rhs)));
    }

    template <typename Box>
    [[nodiscard]] constexpr Box shrU(Box lhs, Box rhs) noexcept
    {
        return Box { static_cast<BitsOf<Box>>(lhs.bits >> shiftCount(rhs)) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box rotl(Box lhs, Box rhs) noexcept
    {
        return Box { std::rotl(lhs.bits, shiftCount(rhs)) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box rotr(Box lhs, Box rhs) noexcept
    {
        return Box { std::rotr(lhs.bits, shiftCount(rhs)) };
    }

    template <typename Box>
    [[nodiscard]] constexpr bool eqz(Box value) noexcept
    {
        return value.bits == 0;
    }

    template <typename Box>
    [[nodiscard]] constexpr bool eq(Box lhs, Box rhs) noexcept
    {
        return lhs.bits == rhs.bits;
    }

    template <typename Box>
    [[nodiscard]] constexpr bool ne(Box lhs, Box rhs) noexcept
    {
        return lhs.bits != rhs.bits;
    }

    template <typename Box>
    [[nodiscard]] constexpr bool ltS(Box lhs, Box rhs) noexcept
    {
        return asSigned(lhs) < asSigned(rhs);
    }

    template <typename Box>
    [[nodiscard]] constexpr bool ltU(Box lhs, Box rhs) noexcept
    {
        return lhs.bits < rhs.bits;
    }

    template <typename Box>
    [[nodiscard]] constexpr bool gtS(Box lhs, Box rhs) noexcept
    {
        return asSigned(lhs) > asSigned(rhs);
    }

    template <typename Box>
    [[nodiscard]] constexpr bool gtU(Box lhs, Box rhs) noexcept
    {
        return lhs.bits > rhs.bits;
    }

    template <typename Box>
    [[nodiscard]] constexpr bool leS(Box lhs, Box rhs) noexcept
    {
        return asSigned(lhs) <= asSigned(rhs);
    }

    template <typename Box>
    [[nodiscard]] constexpr bool leU(Box lhs, Box rhs) noexcept
    {
        return lhs.bits <= rhs.bits;
    }

    template <typename Box>
    [[nodiscard]] constexpr bool geS(Box lhs, Box rhs) noexcept
    {
        return asSigned(lhs) >= asSigned(rhs);
    }

    template <typename Box>
    [[nodiscard]] constexpr bool geU(Box lhs, Box rhs) noexcept
    {
        return lhs.bits >= rhs.bits;
    }

    // --- Float formats ------------------------------------------------------------------------

    /// The IEEE 754 layout of a float value type: F32 (binary32) or F64 (binary64).
    template <typename Box>
    struct FloatFormat;

    template <>
    struct FloatFormat<F32>
    {
        using Bits = uint32_t;
        using Float = float;
        static constexpr int MantissaBits = 23;
        static constexpr int ExponentBias = 127;
        static constexpr Bits CanonicalNan = CanonicalNanF32;
    };

    template <>
    struct FloatFormat<F64>
    {
        using Bits = uint64_t;
        using Float = double;
        static constexpr int MantissaBits = 52;
        static constexpr int ExponentBias = 1023;
        static constexpr Bits CanonicalNan = CanonicalNanF64;
    };

    template <typename Box>
    using FloatBits = FloatFormat<Box>::Bits;

    template <typename Box>
    using FloatOf = FloatFormat<Box>::Float;

    template <typename Box>
    inline constexpr auto SignMask = static_cast<FloatBits<Box>>(
        FloatBits<Box> { 1 } << (std::numeric_limits<FloatBits<Box>>::digits - 1));

    template <typename Box>
    inline constexpr auto MantissaMask =
        static_cast<FloatBits<Box>>((FloatBits<Box> { 1 } << FloatFormat<Box>::MantissaBits) - 1);

    /// The bits of +infinity: the exponent field all ones, the mantissa zero.
    template <typename Box>
    inline constexpr auto InfinityBits = static_cast<FloatBits<Box>>(~SignMask<Box> & ~MantissaMask<Box>);

    /// The bits of 1.0, the biased exponent of zero alone.
    template <typename Box>
    inline constexpr auto OneBits = static_cast<FloatBits<Box>>(
        FloatBits<Box> { FloatFormat<Box>::ExponentBias } << FloatFormat<Box>::MantissaBits);

    /// The bits of 0.5.
    template <typename Box>
    inline constexpr auto HalfBits = static_cast<FloatBits<Box>>(
        FloatBits<Box> { FloatFormat<Box>::ExponentBias - 1 } << FloatFormat<Box>::MantissaBits);

    template <typename Box>
    [[nodiscard]] constexpr FloatBits<Box> magnitudeOf(Box value) noexcept
    {
        return static_cast<FloatBits<Box>>(value.bits & ~SignMask<Box>);
    }

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

    template <typename Box>
    [[nodiscard]] constexpr Box fabs(Box value) noexcept
    {
        return Box { magnitudeOf(value) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box fneg(Box value) noexcept
    {
        return Box { static_cast<FloatBits<Box>>(value.bits ^ SignMask<Box>) };
    }

    template <typename Box>
    [[nodiscard]] constexpr Box copysign(Box lhs, Box rhs) noexcept
    {
        return Box { static_cast<FloatBits<Box>>(magnitudeOf(lhs) | (rhs.bits & SignMask<Box>) ) };
    }

    /// Which way ceil, floor, trunc and nearest round a value that is not integral.
    enum class Rounding : uint8_t
    {
        TowardZero,
        Down,
        Up,
        NearestEven,
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

    template <typename Box>
    [[nodiscard]] constexpr Box ceil(Box value) noexcept
    {
        return roundToIntegral(value, Rounding::Up);
    }

    template <typename Box>
    [[nodiscard]] constexpr Box floor(Box value) noexcept
    {
        return roundToIntegral(value, Rounding::Down);
    }

    template <typename Box>
    [[nodiscard]] constexpr Box trunc(Box value) noexcept
    {
        return roundToIntegral(value, Rounding::TowardZero);
    }

    template <typename Box>
    [[nodiscard]] constexpr Box nearest(Box value) noexcept
    {
        return roundToIntegral(value, Rounding::NearestEven);
    }

    template <typename Box>
    [[nodiscard]] Box sqrt(Box value) noexcept
    {
        return arithmeticResult<Box>(std::sqrt(toHost(value)));
    }

    template <typename Box>
    [[nodiscard]] Box fadd(Box lhs, Box rhs) noexcept
    {
        return arithmeticResult<Box>(toHost(lhs) + toHost(rhs));
    }

    template <typename Box>
    [[nodiscard]] Box fsub(Box lhs, Box rhs) noexcept
    {
        return arithmeticResult<Box>(toHost(lhs) - toHost(rhs));
    }

    template <typename Box>
    [[nodiscard]] Box fmul(Box lhs, Box rhs) noexcept
    {
        return arithmeticResult<Box>(toHost(lhs) * toHost(rhs));
    }

    template <typename Box>
    [[nodiscard]] Box fdiv(Box lhs, Box rhs) noexcept
    {
        return arithmeticResult<Box>(toHost(lhs) / toHost(rhs));
    }

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

    template <typename Box>
    [[nodiscard]] constexpr bool feq(Box lhs, Box rhs) noexcept
    {
        return toHost(lhs) == toHost(rhs);
    }

    template <typename Box>
    [[nodiscard]] constexpr bool fne(Box lhs, Box rhs) noexcept
    {
        return toHost(lhs) != toHost(rhs);
    }

    template <typename Box>
    [[nodiscard]] constexpr bool flt(Box lhs, Box rhs) noexcept
    {
        return toHost(lhs) < toHost(rhs);
    }

    template <typename Box>
    [[nodiscard]] constexpr bool fgt(Box lhs, Box rhs) noexcept
    {
        return toHost(lhs) > toHost(rhs);
    }

    template <typename Box>
    [[nodiscard]] constexpr bool fle(Box lhs, Box rhs) noexcept
    {
        return toHost(lhs) <= toHost(rhs);
    }

    template <typename Box>
    [[nodiscard]] constexpr bool fge(Box lhs, Box rhs) noexcept
    {
        return toHost(lhs) >= toHost(rhs);
    }

    // --- Conversions (spec 4.3.4) -------------------------------------------------------------

    [[nodiscard]] constexpr I32 wrap(I64 value) noexcept
    {
        return I32 { static_cast<uint32_t>(value.bits) };
    }

    [[nodiscard]] constexpr I64 extendS(I32 value) noexcept
    {
        return I64 { static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value.bits))) };
    }

    [[nodiscard]] constexpr I64 extendU(I32 value) noexcept
    {
        return I64 { value.bits };
    }

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
        static constexpr double Min =
            std::is_signed_v<Int> ? -powerOfTwo(std::numeric_limits<Int>::digits) : 0.0;
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
    [[nodiscard]] std::expected<To, Trap> truncTrapping(From value) noexcept
    {
        if (isNan(value))
            return trap(TrapCode::InvalidConversionToInteger);
        auto const integral = std::trunc(toDouble(value));
        if (integral < IntRange<Int>::Min || integral >= IntRange<Int>::MaxPlusOne)
            return trap(TrapCode::IntegerOverflow);
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

    [[nodiscard]] F32 demote(F64 value) noexcept
    {
        if (isNan(value))
            return F32 { CanonicalNanF32 };
        return F32 { std::bit_cast<uint32_t>(static_cast<float>(toHost(value))) };
    }

    [[nodiscard]] F64 promote(F32 value) noexcept
    {
        if (isNan(value))
            return F64 { CanonicalNanF64 };
        return F64 { std::bit_cast<uint64_t>(toDouble(value)) };
    }

    /// reinterpret: the same bits, as the other type of their width.
    template <typename To, typename From>
    [[nodiscard]] constexpr To reinterpret(From value) noexcept
    {
        static_assert(sizeof(To) == sizeof(From));
        return To { value.bits };
    }

    // --- The table ----------------------------------------------------------------------------

    using UnaryFn = Result (*)(Value);
    using BinaryFn = Result (*)(Value, Value);

    /// The parameter and result types of a function pointer.
    template <typename>
    struct Signature;

    template <typename R, typename A>
    struct Signature<R (*)(A) noexcept>
    {
        using Arg = A;
    };

    template <typename R, typename A, typename B>
    struct Signature<R (*)(A, B) noexcept>
    {
        using Lhs = A;
        using Rhs = B;
    };

    /// The operand @p value holds, of type @p T. Precondition (checked): it holds one.
    template <typename T>
    [[nodiscard]] T operand(Value const& value)
    {
        Require(std::holds_alternative<T>(value));
        return std::get<T>(value);
    }

    template <typename T>
    [[nodiscard]] Result lift(T result)
    {
        return Value { result };
    }

    template <typename T>
    [[nodiscard]] Result lift(std::expected<T, Trap> result)
    {
        return result.transform([](T value) { return Value { value }; });
    }

    [[nodiscard]] Result truth(bool holds)
    {
        return Value { I32 { holds ? 1U : 0U } };
    }

    template <auto Fn>
    [[nodiscard]] Result unaryThunk(Value value)
    {
        return lift(Fn(operand<typename Signature<decltype(Fn)>::Arg>(value)));
    }

    template <auto Fn>
    [[nodiscard]] Result binaryThunk(Value lhs, Value rhs)
    {
        using Types = Signature<decltype(Fn)>;
        return lift(Fn(operand<typename Types::Lhs>(lhs), operand<typename Types::Rhs>(rhs)));
    }

    template <auto Fn>
    [[nodiscard]] Result testThunk(Value value)
    {
        return truth(Fn(operand<typename Signature<decltype(Fn)>::Arg>(value)));
    }

    template <auto Fn>
    [[nodiscard]] Result compareThunk(Value lhs, Value rhs)
    {
        using Types = Signature<decltype(Fn)>;
        return truth(Fn(operand<typename Types::Lhs>(lhs), operand<typename Types::Rhs>(rhs)));
    }

    /// One numeric instruction: how it takes its operands, and the function evaluating it.
    struct NumericRow
    {
        Opcode opcode {};                        ///< The instruction.
        NumericShape shape = NumericShape::None; ///< How it takes its operands.
        UnaryFn unary = nullptr;                 ///< Unary and Test: the evaluator.
        BinaryFn binary = nullptr;               ///< Binary and Compare: the evaluator.
    };

    template <auto Fn>
    [[nodiscard]] consteval NumericRow unary(Opcode opcode) noexcept
    {
        return NumericRow { .opcode = opcode, .shape = NumericShape::Unary, .unary = &unaryThunk<Fn> };
    }

    template <auto Fn>
    [[nodiscard]] consteval NumericRow binary(Opcode opcode) noexcept
    {
        return NumericRow { .opcode = opcode, .shape = NumericShape::Binary, .binary = &binaryThunk<Fn> };
    }

    template <auto Fn>
    [[nodiscard]] consteval NumericRow test(Opcode opcode) noexcept
    {
        return NumericRow { .opcode = opcode, .shape = NumericShape::Test, .unary = &testThunk<Fn> };
    }

    template <auto Fn>
    [[nodiscard]] consteval NumericRow compare(Opcode opcode) noexcept
    {
        return NumericRow { .opcode = opcode, .shape = NumericShape::Compare, .binary = &compareThunk<Fn> };
    }

    /// Every numeric instruction, one row each.
    constexpr auto NumericTable = std::to_array<NumericRow>({
        // i32
        test<&eqz<I32>>(Opcode::I32Eqz),
        compare<&eq<I32>>(Opcode::I32Eq),
        compare<&ne<I32>>(Opcode::I32Ne),
        compare<&ltS<I32>>(Opcode::I32LtS),
        compare<&ltU<I32>>(Opcode::I32LtU),
        compare<&gtS<I32>>(Opcode::I32GtS),
        compare<&gtU<I32>>(Opcode::I32GtU),
        compare<&leS<I32>>(Opcode::I32LeS),
        compare<&leU<I32>>(Opcode::I32LeU),
        compare<&geS<I32>>(Opcode::I32GeS),
        compare<&geU<I32>>(Opcode::I32GeU),
        unary<&clz<I32>>(Opcode::I32Clz),
        unary<&ctz<I32>>(Opcode::I32Ctz),
        unary<&popcnt<I32>>(Opcode::I32Popcnt),
        binary<&add<I32>>(Opcode::I32Add),
        binary<&sub<I32>>(Opcode::I32Sub),
        binary<&mul<I32>>(Opcode::I32Mul),
        binary<&divS<I32>>(Opcode::I32DivS),
        binary<&divU<I32>>(Opcode::I32DivU),
        binary<&remS<I32>>(Opcode::I32RemS),
        binary<&remU<I32>>(Opcode::I32RemU),
        binary<&bitAnd<I32>>(Opcode::I32And),
        binary<&bitOr<I32>>(Opcode::I32Or),
        binary<&bitXor<I32>>(Opcode::I32Xor),
        binary<&shl<I32>>(Opcode::I32Shl),
        binary<&shrS<I32>>(Opcode::I32ShrS),
        binary<&shrU<I32>>(Opcode::I32ShrU),
        binary<&rotl<I32>>(Opcode::I32Rotl),
        binary<&rotr<I32>>(Opcode::I32Rotr),
        unary<&extendSigned<I32, int8_t>>(Opcode::I32Extend8S),
        unary<&extendSigned<I32, int16_t>>(Opcode::I32Extend16S),
        // i64
        test<&eqz<I64>>(Opcode::I64Eqz),
        compare<&eq<I64>>(Opcode::I64Eq),
        compare<&ne<I64>>(Opcode::I64Ne),
        compare<&ltS<I64>>(Opcode::I64LtS),
        compare<&ltU<I64>>(Opcode::I64LtU),
        compare<&gtS<I64>>(Opcode::I64GtS),
        compare<&gtU<I64>>(Opcode::I64GtU),
        compare<&leS<I64>>(Opcode::I64LeS),
        compare<&leU<I64>>(Opcode::I64LeU),
        compare<&geS<I64>>(Opcode::I64GeS),
        compare<&geU<I64>>(Opcode::I64GeU),
        unary<&clz<I64>>(Opcode::I64Clz),
        unary<&ctz<I64>>(Opcode::I64Ctz),
        unary<&popcnt<I64>>(Opcode::I64Popcnt),
        binary<&add<I64>>(Opcode::I64Add),
        binary<&sub<I64>>(Opcode::I64Sub),
        binary<&mul<I64>>(Opcode::I64Mul),
        binary<&divS<I64>>(Opcode::I64DivS),
        binary<&divU<I64>>(Opcode::I64DivU),
        binary<&remS<I64>>(Opcode::I64RemS),
        binary<&remU<I64>>(Opcode::I64RemU),
        binary<&bitAnd<I64>>(Opcode::I64And),
        binary<&bitOr<I64>>(Opcode::I64Or),
        binary<&bitXor<I64>>(Opcode::I64Xor),
        binary<&shl<I64>>(Opcode::I64Shl),
        binary<&shrS<I64>>(Opcode::I64ShrS),
        binary<&shrU<I64>>(Opcode::I64ShrU),
        binary<&rotl<I64>>(Opcode::I64Rotl),
        binary<&rotr<I64>>(Opcode::I64Rotr),
        unary<&extendSigned<I64, int8_t>>(Opcode::I64Extend8S),
        unary<&extendSigned<I64, int16_t>>(Opcode::I64Extend16S),
        unary<&extendSigned<I64, int32_t>>(Opcode::I64Extend32S),
        // f32
        compare<&feq<F32>>(Opcode::F32Eq),
        compare<&fne<F32>>(Opcode::F32Ne),
        compare<&flt<F32>>(Opcode::F32Lt),
        compare<&fgt<F32>>(Opcode::F32Gt),
        compare<&fle<F32>>(Opcode::F32Le),
        compare<&fge<F32>>(Opcode::F32Ge),
        unary<&fabs<F32>>(Opcode::F32Abs),
        unary<&fneg<F32>>(Opcode::F32Neg),
        unary<&ceil<F32>>(Opcode::F32Ceil),
        unary<&floor<F32>>(Opcode::F32Floor),
        unary<&trunc<F32>>(Opcode::F32Trunc),
        unary<&nearest<F32>>(Opcode::F32Nearest),
        unary<&sqrt<F32>>(Opcode::F32Sqrt),
        binary<&fadd<F32>>(Opcode::F32Add),
        binary<&fsub<F32>>(Opcode::F32Sub),
        binary<&fmul<F32>>(Opcode::F32Mul),
        binary<&fdiv<F32>>(Opcode::F32Div),
        binary<&fmin<F32>>(Opcode::F32Min),
        binary<&fmax<F32>>(Opcode::F32Max),
        binary<&copysign<F32>>(Opcode::F32Copysign),
        // f64
        compare<&feq<F64>>(Opcode::F64Eq),
        compare<&fne<F64>>(Opcode::F64Ne),
        compare<&flt<F64>>(Opcode::F64Lt),
        compare<&fgt<F64>>(Opcode::F64Gt),
        compare<&fle<F64>>(Opcode::F64Le),
        compare<&fge<F64>>(Opcode::F64Ge),
        unary<&fabs<F64>>(Opcode::F64Abs),
        unary<&fneg<F64>>(Opcode::F64Neg),
        unary<&ceil<F64>>(Opcode::F64Ceil),
        unary<&floor<F64>>(Opcode::F64Floor),
        unary<&trunc<F64>>(Opcode::F64Trunc),
        unary<&nearest<F64>>(Opcode::F64Nearest),
        unary<&sqrt<F64>>(Opcode::F64Sqrt),
        binary<&fadd<F64>>(Opcode::F64Add),
        binary<&fsub<F64>>(Opcode::F64Sub),
        binary<&fmul<F64>>(Opcode::F64Mul),
        binary<&fdiv<F64>>(Opcode::F64Div),
        binary<&fmin<F64>>(Opcode::F64Min),
        binary<&fmax<F64>>(Opcode::F64Max),
        binary<&copysign<F64>>(Opcode::F64Copysign),
        // conversions
        unary<&wrap>(Opcode::I32WrapI64),
        unary<&truncTrapping<I32, int32_t, F32>>(Opcode::I32TruncF32S),
        unary<&truncTrapping<I32, uint32_t, F32>>(Opcode::I32TruncF32U),
        unary<&truncTrapping<I32, int32_t, F64>>(Opcode::I32TruncF64S),
        unary<&truncTrapping<I32, uint32_t, F64>>(Opcode::I32TruncF64U),
        unary<&extendS>(Opcode::I64ExtendI32S),
        unary<&extendU>(Opcode::I64ExtendI32U),
        unary<&truncTrapping<I64, int64_t, F32>>(Opcode::I64TruncF32S),
        unary<&truncTrapping<I64, uint64_t, F32>>(Opcode::I64TruncF32U),
        unary<&truncTrapping<I64, int64_t, F64>>(Opcode::I64TruncF64S),
        unary<&truncTrapping<I64, uint64_t, F64>>(Opcode::I64TruncF64U),
        unary<&convert<F32, int32_t, I32>>(Opcode::F32ConvertI32S),
        unary<&convert<F32, uint32_t, I32>>(Opcode::F32ConvertI32U),
        unary<&convert<F32, int64_t, I64>>(Opcode::F32ConvertI64S),
        unary<&convert<F32, uint64_t, I64>>(Opcode::F32ConvertI64U),
        unary<&demote>(Opcode::F32DemoteF64),
        unary<&convert<F64, int32_t, I32>>(Opcode::F64ConvertI32S),
        unary<&convert<F64, uint32_t, I32>>(Opcode::F64ConvertI32U),
        unary<&convert<F64, int64_t, I64>>(Opcode::F64ConvertI64S),
        unary<&convert<F64, uint64_t, I64>>(Opcode::F64ConvertI64U),
        unary<&promote>(Opcode::F64PromoteF32),
        unary<&reinterpret<I32, F32>>(Opcode::I32ReinterpretF32),
        unary<&reinterpret<I64, F64>>(Opcode::I64ReinterpretF64),
        unary<&reinterpret<F32, I32>>(Opcode::F32ReinterpretI32),
        unary<&reinterpret<F64, I64>>(Opcode::F64ReinterpretI64),
        unary<&truncSaturating<I32, int32_t, F32>>(Opcode::I32TruncSatF32S),
        unary<&truncSaturating<I32, uint32_t, F32>>(Opcode::I32TruncSatF32U),
        unary<&truncSaturating<I32, int32_t, F64>>(Opcode::I32TruncSatF64S),
        unary<&truncSaturating<I32, uint32_t, F64>>(Opcode::I32TruncSatF64U),
        unary<&truncSaturating<I64, int64_t, F32>>(Opcode::I64TruncSatF32S),
        unary<&truncSaturating<I64, uint64_t, F32>>(Opcode::I64TruncSatF32U),
        unary<&truncSaturating<I64, int64_t, F64>>(Opcode::I64TruncSatF64S),
        unary<&truncSaturating<I64, uint64_t, F64>>(Opcode::I64TruncSatF64U),
    });

    /// NumericTable indexed by opcode, for a constant-time lookup; None for every other opcode.
    constexpr auto NumericByOpcode = [] {
        auto table = std::array<NumericRow, binary::OpcodeCount> {};
        for (auto const& row: NumericTable)
            table.at(static_cast<std::size_t>(row.opcode)) = row;
        return table;
    }();

    /// The row of @p opcode. Precondition (checked): it is a numeric instruction of @p shape.
    [[nodiscard]] NumericRow const& rowOf(Opcode opcode, NumericShape shape)
    {
        auto const& row = NumericByOpcode[static_cast<std::size_t>(opcode)];
        Require(row.shape == shape);
        return row;
    }
} // namespace

NumericShape numericShape(Opcode opcode) noexcept
{
    return NumericByOpcode[static_cast<std::size_t>(opcode)].shape;
}

std::expected<Value, Trap> applyUnary(Opcode opcode, Value operand)
{
    return rowOf(opcode, NumericShape::Unary).unary(operand);
}

std::expected<Value, Trap> applyBinary(Opcode opcode, Value lhs, Value rhs)
{
    return rowOf(opcode, NumericShape::Binary).binary(lhs, rhs);
}

I32 applyTest(Opcode opcode, Value operand)
{
    return std::get<I32>(*rowOf(opcode, NumericShape::Test).unary(operand));
}

I32 applyCompare(Opcode opcode, Value lhs, Value rhs)
{
    return std::get<I32>(*rowOf(opcode, NumericShape::Compare).binary(lhs, rhs));
}

} // namespace isoline::interp
