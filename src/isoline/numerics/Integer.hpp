// SPDX-License-Identifier: Apache-2.0
#pragma once

// The typed integer kernels of WebAssembly's numeric instructions (spec 4.3.2), the conversions
// between the two integer types (spec 4.3.4), and wide arithmetic: one template per operator over
// the operand's value type, with no boxed runtime::Value and no dispatch. Every executor evaluates
// integer instructions through them, and every one is constexpr, so a compiler may fold with them.

#include <array>
#include <bit>
#include <cstdint>
#include <expected>
#include <limits>

#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::numerics
{

// --- Integer formats ----------------------------------------------------------------------

/// The representation of an integer value type: I32 or I64.
template <typename Box>
struct IntFormat;

/// i32: 32 bits, signed or not as the instruction decides.
template <>
struct IntFormat<runtime::I32>
{
    using Bits = uint32_t;  ///< The bits, unsigned.
    using Signed = int32_t; ///< The bits, read as two's complement.
};

/// i64: 64 bits, signed or not as the instruction decides.
template <>
struct IntFormat<runtime::I64>
{
    using Bits = uint64_t;  ///< The bits, unsigned.
    using Signed = int64_t; ///< The bits, read as two's complement.
};

/// The unsigned type holding the bits of an integer value type.
template <typename Box>
using BitsOf = IntFormat<Box>::Bits;

/// The signed type of an integer value type's width.
template <typename Box>
using SignedOf = IntFormat<Box>::Signed;

/// The bit width of an integer value type.
template <typename Box>
inline constexpr auto WidthOf = static_cast<BitsOf<Box>>(std::numeric_limits<BitsOf<Box>>::digits);

/// The bits of @p value, read as a two's complement signed integer.
template <typename Box>
[[nodiscard]] constexpr SignedOf<Box> asSigned(Box value) noexcept
{
    return static_cast<SignedOf<Box>>(value.bits);
}

/// The integer value type holding the two's complement bits of @p value.
template <typename Box>
[[nodiscard]] constexpr Box fromSigned(SignedOf<Box> value) noexcept
{
    return Box { static_cast<BitsOf<Box>>(value) };
}

// --- Integer operators (spec 4.3.2) -------------------------------------------------------

/// i32.clz and i64.clz: the number of leading zero bits.
template <typename Box>
[[nodiscard]] constexpr Box clz(Box value) noexcept
{
    return Box { static_cast<BitsOf<Box>>(std::countl_zero(value.bits)) };
}

/// i32.ctz and i64.ctz: the number of trailing zero bits.
template <typename Box>
[[nodiscard]] constexpr Box ctz(Box value) noexcept
{
    return Box { static_cast<BitsOf<Box>>(std::countr_zero(value.bits)) };
}

/// i32.popcnt and i64.popcnt: the number of bits set.
template <typename Box>
[[nodiscard]] constexpr Box popcnt(Box value) noexcept
{
    return Box { static_cast<BitsOf<Box>>(std::popcount(value.bits)) };
}

/// i32.extend8_s, i32.extend16_s and i64.extend8/16/32_s: the low bits of @p value that @p Narrow
/// holds, sign-extended.
template <typename Box, typename Narrow>
[[nodiscard]] constexpr Box extendSigned(Box value) noexcept
{
    return fromSigned<Box>(static_cast<Narrow>(value.bits));
}

/// i32.add and i64.add: the sum modulo 2^N.
template <typename Box>
[[nodiscard]] constexpr Box add(Box lhs, Box rhs) noexcept
{
    return Box { static_cast<BitsOf<Box>>(lhs.bits + rhs.bits) };
}

/// i32.sub and i64.sub: the difference modulo 2^N.
template <typename Box>
[[nodiscard]] constexpr Box sub(Box lhs, Box rhs) noexcept
{
    return Box { static_cast<BitsOf<Box>>(lhs.bits - rhs.bits) };
}

/// i32.mul and i64.mul: the product modulo 2^N.
template <typename Box>
[[nodiscard]] constexpr Box mul(Box lhs, Box rhs) noexcept
{
    return Box { static_cast<BitsOf<Box>>(lhs.bits * rhs.bits) };
}

/// div_s: the quotient rounded toward zero; traps on a zero divisor, and on min / -1, which overflows.
template <typename Box>
[[nodiscard]] constexpr std::expected<Box, runtime::Trap> divS(Box lhs, Box rhs) noexcept
{
    if (rhs.bits == 0)
        return runtime::trap(runtime::TrapCode::IntegerDivideByZero);
    if (asSigned(lhs) == std::numeric_limits<SignedOf<Box>>::min() && asSigned(rhs) == -1)
        return runtime::trap(runtime::TrapCode::IntegerOverflow);
    return fromSigned<Box>(asSigned(lhs) / asSigned(rhs));
}

/// div_u: the unsigned quotient; traps on a zero divisor.
template <typename Box>
[[nodiscard]] constexpr std::expected<Box, runtime::Trap> divU(Box lhs, Box rhs) noexcept
{
    if (rhs.bits == 0)
        return runtime::trap(runtime::TrapCode::IntegerDivideByZero);
    return Box { static_cast<BitsOf<Box>>(lhs.bits / rhs.bits) };
}

/// The remainder, signed as the dividend; min rem -1 is 0, where the host would overflow.
template <typename Box>
[[nodiscard]] constexpr std::expected<Box, runtime::Trap> remS(Box lhs, Box rhs) noexcept
{
    if (rhs.bits == 0)
        return runtime::trap(runtime::TrapCode::IntegerDivideByZero);
    if (asSigned(rhs) == -1)
        return Box {};
    return fromSigned<Box>(asSigned(lhs) % asSigned(rhs));
}

/// rem_u: the unsigned remainder; traps on a zero divisor.
template <typename Box>
[[nodiscard]] constexpr std::expected<Box, runtime::Trap> remU(Box lhs, Box rhs) noexcept
{
    if (rhs.bits == 0)
        return runtime::trap(runtime::TrapCode::IntegerDivideByZero);
    return Box { static_cast<BitsOf<Box>>(lhs.bits % rhs.bits) };
}

/// and: the bitwise conjunction.
template <typename Box>
[[nodiscard]] constexpr Box bitAnd(Box lhs, Box rhs) noexcept
{
    return Box { static_cast<BitsOf<Box>>(lhs.bits & rhs.bits) };
}

/// or: the bitwise disjunction.
template <typename Box>
[[nodiscard]] constexpr Box bitOr(Box lhs, Box rhs) noexcept
{
    return Box { static_cast<BitsOf<Box>>(lhs.bits | rhs.bits) };
}

/// xor: the bitwise exclusive disjunction.
template <typename Box>
[[nodiscard]] constexpr Box bitXor(Box lhs, Box rhs) noexcept
{
    return Box { static_cast<BitsOf<Box>>(lhs.bits ^ rhs.bits) };
}

namespace detail
{
    /// The shift count: the operand modulo the bit width.
    template <typename Box>
    [[nodiscard]] constexpr int shiftCount(Box count) noexcept
    {
        return static_cast<int>(count.bits % WidthOf<Box>);
    }
} // namespace detail

/// shl: the left shift by the count modulo the bit width.
template <typename Box>
[[nodiscard]] constexpr Box shl(Box lhs, Box rhs) noexcept
{
    return Box { static_cast<BitsOf<Box>>(lhs.bits << detail::shiftCount(rhs)) };
}

/// The arithmetic right shift: C++20 defines >> of a negative value as one.
template <typename Box>
[[nodiscard]] constexpr Box shrS(Box lhs, Box rhs) noexcept
{
    return fromSigned<Box>(static_cast<SignedOf<Box>>(asSigned(lhs) >> detail::shiftCount(rhs)));
}

/// shr_u: the logical right shift by the count modulo the bit width.
template <typename Box>
[[nodiscard]] constexpr Box shrU(Box lhs, Box rhs) noexcept
{
    return Box { static_cast<BitsOf<Box>>(lhs.bits >> detail::shiftCount(rhs)) };
}

/// rotl: the left rotation by the count modulo the bit width.
template <typename Box>
[[nodiscard]] constexpr Box rotl(Box lhs, Box rhs) noexcept
{
    return Box { std::rotl(lhs.bits, detail::shiftCount(rhs)) };
}

/// rotr: the right rotation by the count modulo the bit width.
template <typename Box>
[[nodiscard]] constexpr Box rotr(Box lhs, Box rhs) noexcept
{
    return Box { std::rotr(lhs.bits, detail::shiftCount(rhs)) };
}

/// eqz: whether @p value is zero.
template <typename Box>
[[nodiscard]] constexpr bool eqz(Box value) noexcept
{
    return value.bits == 0;
}

/// eq: whether both operands have the same bits.
template <typename Box>
[[nodiscard]] constexpr bool eq(Box lhs, Box rhs) noexcept
{
    return lhs.bits == rhs.bits;
}

/// ne: whether the operands' bits differ.
template <typename Box>
[[nodiscard]] constexpr bool ne(Box lhs, Box rhs) noexcept
{
    return lhs.bits != rhs.bits;
}

/// lt_s: the signed less-than.
template <typename Box>
[[nodiscard]] constexpr bool ltS(Box lhs, Box rhs) noexcept
{
    return asSigned(lhs) < asSigned(rhs);
}

/// lt_u: the unsigned less-than.
template <typename Box>
[[nodiscard]] constexpr bool ltU(Box lhs, Box rhs) noexcept
{
    return lhs.bits < rhs.bits;
}

/// gt_s: the signed greater-than.
template <typename Box>
[[nodiscard]] constexpr bool gtS(Box lhs, Box rhs) noexcept
{
    return asSigned(lhs) > asSigned(rhs);
}

/// gt_u: the unsigned greater-than.
template <typename Box>
[[nodiscard]] constexpr bool gtU(Box lhs, Box rhs) noexcept
{
    return lhs.bits > rhs.bits;
}

/// le_s: the signed less-or-equal.
template <typename Box>
[[nodiscard]] constexpr bool leS(Box lhs, Box rhs) noexcept
{
    return asSigned(lhs) <= asSigned(rhs);
}

/// le_u: the unsigned less-or-equal.
template <typename Box>
[[nodiscard]] constexpr bool leU(Box lhs, Box rhs) noexcept
{
    return lhs.bits <= rhs.bits;
}

/// ge_s: the signed greater-or-equal.
template <typename Box>
[[nodiscard]] constexpr bool geS(Box lhs, Box rhs) noexcept
{
    return asSigned(lhs) >= asSigned(rhs);
}

/// ge_u: the unsigned greater-or-equal.
template <typename Box>
[[nodiscard]] constexpr bool geU(Box lhs, Box rhs) noexcept
{
    return lhs.bits >= rhs.bits;
}

// --- Conversions (spec 4.3.4) -------------------------------------------------------------

/// i32.wrap_i64: the low 32 bits.
[[nodiscard]] constexpr runtime::I32 wrap(runtime::I64 value) noexcept
{
    return runtime::I32 { static_cast<uint32_t>(value.bits) };
}

/// i64.extend_i32_s: the value sign-extended to 64 bits.
[[nodiscard]] constexpr runtime::I64 extendS(runtime::I32 value) noexcept
{
    return runtime::I64 { static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value.bits))) };
}

/// i64.extend_i32_u: the value zero-extended to 64 bits.
[[nodiscard]] constexpr runtime::I64 extendU(runtime::I32 value) noexcept
{
    return runtime::I64 { value.bits };
}

// --- Wide arithmetic ----------------------------------------------------------------------
//
// 128-bit values as two 64-bit limbs, {low, high}, in plain 64-bit arithmetic: no __int128 and no
// intrinsic, so every compiler computes the same thing the same way.

/// A 128-bit value: its low half, then its high half.
using Halves = std::array<uint64_t, 2>;

/// i64.add128: the sum modulo 2^128; the carry out of the low half goes into the high one.
[[nodiscard]] constexpr Halves add128(Halves lhs, Halves rhs) noexcept
{
    auto const low = lhs[0] + rhs[0];
    auto const carry = low < lhs[0] ? uint64_t { 1 } : uint64_t { 0 };
    return Halves { low, lhs[1] + rhs[1] + carry };
}

/// i64.sub128: the difference modulo 2^128; the borrow of the low half comes out of the high one.
[[nodiscard]] constexpr Halves sub128(Halves lhs, Halves rhs) noexcept
{
    auto const borrow = lhs[0] < rhs[0] ? uint64_t { 1 } : uint64_t { 0 };
    return Halves { lhs[0] - rhs[0], lhs[1] - rhs[1] - borrow };
}

/// i64.mul_wide_u: the full 128-bit product of two unsigned 64-bit values, by schoolbook
/// multiplication of their 32-bit halves, none of whose partial products overflows 64 bits.
[[nodiscard]] constexpr Halves mulWideU(uint64_t lhs, uint64_t rhs) noexcept
{
    constexpr auto Mask = uint64_t { 0xFFFF'FFFF };
    auto const lhsLow = lhs & Mask;
    auto const lhsHigh = lhs >> 32U;
    auto const rhsLow = rhs & Mask;
    auto const rhsHigh = rhs >> 32U;
    auto const lowLow = lhsLow * rhsLow;
    auto const lowHigh = lhsLow * rhsHigh;
    auto const highLow = lhsHigh * rhsLow;
    auto const highHigh = lhsHigh * rhsHigh;
    // The product's bits from 32 up, as far as the low product's high half and the cross terms'
    // low halves reach them: at most 3 * (2^32 - 1), so the sum does not overflow. Its low half
    // completes the low limb, its high half carries into the high limb.
    auto const middle = (lowLow >> 32U) + (lowHigh & Mask) + (highLow & Mask);
    return Halves { (middle << 32U) | (lowLow & Mask),
                    highHigh + (lowHigh >> 32U) + (highLow >> 32U) + (middle >> 32U) };
}

/// i64.mul_wide_s: the full 128-bit product of two signed 64-bit values. As two's complement
/// x = u - 2^64 * sign(x), the signed product's high half is the unsigned one's less each
/// operand that the other's sign bit scales.
[[nodiscard]] constexpr Halves mulWideS(uint64_t lhs, uint64_t rhs) noexcept
{
    auto const unsignedProduct = mulWideU(lhs, rhs);
    auto const signBit = uint64_t { 1 } << 63U;
    auto const lhsCorrection = (lhs & signBit) != 0 ? rhs : uint64_t { 0 };
    auto const rhsCorrection = (rhs & signBit) != 0 ? lhs : uint64_t { 0 };
    return Halves { unsignedProduct[0], unsignedProduct[1] - lhsCorrection - rhsCorrection };
}

} // namespace isoline::numerics
