// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <ranges>
#include <type_traits>

#include <isoline/binary/Opcode.hpp>
#include <isoline/numerics/Float.hpp>
#include <isoline/numerics/Integer.hpp>
#include <isoline/numerics/SimdKernels.hpp>
#include <isoline/numerics/SimdLanes.hpp>
#include <isoline/numerics/SimdShape.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::binary::Opcode;
using isoline::binary::OpcodeSignature;
using isoline::binary::OpcodeTable;
using isoline::numerics::SimdShape;
using isoline::numerics::simdShape;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::V128;

namespace numerics = isoline::numerics;

namespace
{

// --- Scalar references --------------------------------------------------------------------
//
// Each lane-wise kernel row is checked against the scalar numerics kernel of its instruction, applied
// to one lane at a time. A lane narrower than 32 bits is widened into an i32 (zero- or sign-extended,
// as the instruction reads it) and the result's low bits taken, which is what wrapping, comparing or
// saturating at the lane's width means.

/// The low @p Width bits of @p value.
template <unsigned Width>
constexpr uint64_t lowBits(uint64_t value) noexcept
{
    if constexpr (Width == 64)
        return value;
    else
        return value & ((uint64_t { 1 } << Width) - 1);
}

/// The scalar integer type a lane of @p Width bits is computed in.
template <unsigned Width>
using IntBox = std::conditional_t<Width <= 32, I32, I64>;

/// Which way an integer instruction reads its lanes.
enum class Reading : uint8_t
{
    Unsigned, ///< As unsigned: zero-extended.
    Signed,   ///< As signed: sign-extended.
};

/// A lane of @p Width bits, extended into its scalar type as @p How reads it.
template <unsigned Width, Reading How = Reading::Unsigned>
constexpr IntBox<Width> extended(uint64_t lane) noexcept
{
    using Box = IntBox<Width>;
    auto const zeroExtended = Box { static_cast<numerics::BitsOf<Box>>(lane) };
    if constexpr (How == Reading::Signed)
    {
        constexpr auto Shift = Box { numerics::WidthOf<Box> - Width };
        return numerics::shrS(numerics::shl(zeroExtended, Shift), Shift);
    }
    else
        return zeroExtended;
}

constexpr auto Unsigned = Reading::Unsigned;
constexpr auto Signed = Reading::Signed;

/// The integer kernel @p Kernel on two lanes, its result truncated to the lane.
template <unsigned Width, auto Kernel>
uint64_t integerBinary(uint64_t lhs, uint64_t rhs)
{
    return lowBits<Width>(Kernel(extended<Width>(lhs), extended<Width>(rhs)).bits);
}

/// The integer comparison @p Kernel on two lanes: all ones where it holds.
template <unsigned Width, auto Kernel, Reading How>
uint64_t integerCompare(uint64_t lhs, uint64_t rhs)
{
    return Kernel(extended<Width, How>(lhs), extended<Width, How>(rhs)) ? lowBits<Width>(~uint64_t { 0 }) : 0;
}

/// The lane @p Relation prefers: lhs where it holds, else rhs (min and max).
template <unsigned Width, auto Relation, Reading How>
uint64_t integerPick(uint64_t lhs, uint64_t rhs)
{
    return Relation(extended<Width, How>(lhs), extended<Width, How>(rhs)) ? lhs : rhs;
}

template <unsigned Width>
uint64_t integerNeg(uint64_t lane)
{
    return lowBits<Width>(numerics::sub(IntBox<Width> {}, extended<Width>(lane)).bits);
}

template <unsigned Width>
uint64_t integerAbs(uint64_t lane)
{
    return numerics::ltS(extended<Width, Signed>(lane), IntBox<Width> {}) ? integerNeg<Width>(lane) : lane;
}

template <unsigned Width>
uint64_t integerPopcnt(uint64_t lane)
{
    return numerics::popcnt(extended<Width>(lane)).bits;
}

/// v128.not, on a 64-bit half: xor with all ones.
uint64_t bitwiseNot(uint64_t half)
{
    return numerics::bitXor(I64 { half }, I64 { ~uint64_t { 0 } }).bits;
}

/// v128.andnot, on a 64-bit half.
uint64_t bitwiseAndNot(uint64_t lhs, uint64_t rhs)
{
    return numerics::bitAnd(I64 { lhs }, I64 { bitwiseNot(rhs) }).bits;
}

/// avgr_u: (a + b + 1) / 2, exact in an i32 for lanes of 16 bits or fewer.
template <unsigned Width>
uint64_t averageRounded(uint64_t lhs, uint64_t rhs)
{
    auto const sum = numerics::add(numerics::add(extended<Width>(lhs), extended<Width>(rhs)), I32 { 1 });
    return numerics::shrU(sum, I32 { 1 }).bits;
}

/// @p exact, a signed i32, clamped to the lane's range read per @p How.
template <unsigned Width, Reading How>
uint64_t clamped(I32 exact)
{
    auto const value = int64_t { numerics::asSigned(exact) };
    auto const low = How == Reading::Signed ? -(int64_t { 1 } << (Width - 1)) : int64_t { 0 };
    auto const high =
        How == Reading::Signed ? (int64_t { 1 } << (Width - 1)) - 1 : (int64_t { 1 } << Width) - 1;
    return lowBits<Width>(static_cast<uint64_t>(std::clamp(value, low, high)));
}

/// add_sat and sub_sat: the exact i32 sum or difference, saturated to the lane.
template <unsigned Width, auto Kernel, Reading How>
uint64_t saturating(uint64_t lhs, uint64_t rhs)
{
    return clamped<Width, How>(Kernel(extended<Width, How>(lhs), extended<Width, How>(rhs)));
}

/// q15mulr_sat_s: (a * b + 0x4000) >> 15, saturated; the product of two i16 fits an i32.
uint64_t q15MulRound(uint64_t lhs, uint64_t rhs)
{
    auto const product = numerics::mul(extended<16, Signed>(lhs), extended<16, Signed>(rhs));
    return clamped<16, Signed>(numerics::shrS(numerics::add(product, I32 { 0x4000 }), I32 { 15 }));
}

/// The float kernel @p Kernel on two lanes.
template <typename Box, auto Kernel>
uint64_t floatBinary(uint64_t lhs, uint64_t rhs)
{
    using Bits = numerics::FloatBits<Box>;
    return Kernel(Box { static_cast<Bits>(lhs) }, Box { static_cast<Bits>(rhs) }).bits;
}

/// The float comparison @p Kernel on two lanes: all ones where it holds.
template <typename Box, auto Kernel>
uint64_t floatCompare(uint64_t lhs, uint64_t rhs)
{
    using Bits = numerics::FloatBits<Box>;
    return Kernel(Box { static_cast<Bits>(lhs) }, Box { static_cast<Bits>(rhs) })
               ? uint64_t { std::numeric_limits<Bits>::max() }
               : 0;
}

/// pmin, the spec's definition on lt: rhs < lhs ? rhs : lhs, an operand's bits, a NaN's included.
template <typename Box>
uint64_t pseudoMinimum(uint64_t lhs, uint64_t rhs)
{
    using Bits = numerics::FloatBits<Box>;
    return numerics::flt(Box { static_cast<Bits>(rhs) }, Box { static_cast<Bits>(lhs) }) ? rhs : lhs;
}

/// pmax: lhs < rhs ? rhs : lhs.
template <typename Box>
uint64_t pseudoMaximum(uint64_t lhs, uint64_t rhs)
{
    using Bits = numerics::FloatBits<Box>;
    return numerics::flt(Box { static_cast<Bits>(lhs) }, Box { static_cast<Bits>(rhs) }) ? rhs : lhs;
}

/// The unary kernel @p Kernel from a lane of type @p From to a lane of the type it returns.
template <typename From, auto Kernel>
uint64_t scalarUnary(uint64_t lane)
{
    return Kernel(From { static_cast<decltype(From {}.bits)>(lane) }).bits;
}

/// Narrow lane @p part (0 the lowest bits) of the wide lane @p lane.
template <unsigned Narrow>
constexpr uint64_t partOf(uint64_t lane, unsigned part) noexcept
{
    return lowBits<Narrow>(lane >> (part * Narrow));
}

/// dot and relaxed_dot of one wide lane: the signed products of its two narrow parts, summed and
/// wrapped at the lane's width.
template <unsigned Narrow, unsigned Wide>
uint64_t pairDot(uint64_t lhs, uint64_t rhs)
{
    auto const product = [&](unsigned part) {
        return numerics::mul(extended<Narrow, Signed>(partOf<Narrow>(lhs, part)),
                             extended<Narrow, Signed>(partOf<Narrow>(rhs, part)));
    };
    return lowBits<Wide>(numerics::add(product(0), product(1)).bits);
}

/// extadd_pairwise of one wide lane: its two narrow parts, read per @p How, summed.
template <unsigned Narrow, unsigned Wide, Reading How>
uint64_t pairSum(uint64_t lane)
{
    return lowBits<Wide>(numerics::add(extended<Narrow, How>(partOf<Narrow>(lane, 0)),
                                       extended<Narrow, How>(partOf<Narrow>(lane, 1)))
                             .bits);
}

/// The shift kernel @p Kernel on a lane read per @p How, by @p count modulo the lane width.
template <unsigned Width, auto Kernel, Reading How>
uint64_t shifted(uint64_t lane, uint32_t count)
{
    using Box = IntBox<Width>;
    auto const distance = Box { static_cast<numerics::BitsOf<Box>>(count % Width) };
    return lowBits<Width>(Kernel(extended<Width, How>(lane), distance).bits);
}

// --- The rows -----------------------------------------------------------------------------

/// A lane-wise kernel and the scalar reference of each of its lanes. Lane i of the result is the
/// reference of lane i of the operands; where the operand and result lanes differ in width, the
/// lower lanes are converted, as many as the wider shape has, and the rest of the result is zero.
struct LanewiseRow
{
    Opcode opcode {};                                 ///< The instruction.
    unsigned operandBits = 0;                         ///< The width of an operand lane.
    unsigned resultBits = 0;                          ///< The width of a result lane.
    uint64_t (*unary)(uint64_t) = nullptr;            ///< Unary rows: the reference.
    uint64_t (*binary)(uint64_t, uint64_t) = nullptr; ///< Binary rows: the reference.
    uint64_t (*shift)(uint64_t, uint32_t) = nullptr;  ///< Shift rows: the reference.
};

template <unsigned Width, uint64_t (*Reference)(uint64_t, uint64_t)>
constexpr LanewiseRow binaryRow(Opcode opcode)
{
    return LanewiseRow { .opcode = opcode, .operandBits = Width, .resultBits = Width, .binary = Reference };
}

template <unsigned From, unsigned To, uint64_t (*Reference)(uint64_t)>
constexpr LanewiseRow unaryRow(Opcode opcode)
{
    return LanewiseRow { .opcode = opcode, .operandBits = From, .resultBits = To, .unary = Reference };
}

template <unsigned Width, uint64_t (*Reference)(uint64_t, uint32_t)>
constexpr LanewiseRow shiftRow(Opcode opcode)
{
    return LanewiseRow { .opcode = opcode, .operandBits = Width, .resultBits = Width, .shift = Reference };
}

/// The rows every integer lane width has: add, sub, neg, abs and the ten comparisons.
template <unsigned W>
constexpr auto integerRows(std::array<Opcode, 4> const& arithmetic, std::array<Opcode, 10> const& compare)
{
    using Box = IntBox<W>;
    return std::to_array<LanewiseRow>({
        binaryRow<W, &integerBinary<W, &numerics::add<Box>>>(arithmetic[0]),
        binaryRow<W, &integerBinary<W, &numerics::sub<Box>>>(arithmetic[1]),
        unaryRow<W, W, &integerNeg<W>>(arithmetic[2]),
        unaryRow<W, W, &integerAbs<W>>(arithmetic[3]),
        binaryRow<W, &integerCompare<W, &numerics::eq<Box>, Unsigned>>(compare[0]),
        binaryRow<W, &integerCompare<W, &numerics::ne<Box>, Unsigned>>(compare[1]),
        binaryRow<W, &integerCompare<W, &numerics::ltS<Box>, Signed>>(compare[2]),
        binaryRow<W, &integerCompare<W, &numerics::ltU<Box>, Unsigned>>(compare[3]),
        binaryRow<W, &integerCompare<W, &numerics::gtS<Box>, Signed>>(compare[4]),
        binaryRow<W, &integerCompare<W, &numerics::gtU<Box>, Unsigned>>(compare[5]),
        binaryRow<W, &integerCompare<W, &numerics::leS<Box>, Signed>>(compare[6]),
        binaryRow<W, &integerCompare<W, &numerics::leU<Box>, Unsigned>>(compare[7]),
        binaryRow<W, &integerCompare<W, &numerics::geS<Box>, Signed>>(compare[8]),
        binaryRow<W, &integerCompare<W, &numerics::geU<Box>, Unsigned>>(compare[9]),
    });
}

/// The shift rows of one lane width: shl, shr_s, shr_u.
template <unsigned W>
constexpr auto shiftRows(std::array<Opcode, 3> const& opcodes)
{
    using Box = IntBox<W>;
    return std::to_array<LanewiseRow>({
        shiftRow<W, &shifted<W, &numerics::shl<Box>, Unsigned>>(opcodes[0]),
        shiftRow<W, &shifted<W, &numerics::shrS<Box>, Signed>>(opcodes[1]),
        shiftRow<W, &shifted<W, &numerics::shrU<Box>, Unsigned>>(opcodes[2]),
    });
}

/// The minimum and maximum rows of one lane width: min_s, min_u, max_s, max_u.
template <unsigned W>
constexpr auto pickRows(std::array<Opcode, 4> const& opcodes)
{
    using Box = IntBox<W>;
    return std::to_array<LanewiseRow>({
        binaryRow<W, &integerPick<W, &numerics::ltS<Box>, Signed>>(opcodes[0]),
        binaryRow<W, &integerPick<W, &numerics::ltU<Box>, Unsigned>>(opcodes[1]),
        binaryRow<W, &integerPick<W, &numerics::gtS<Box>, Signed>>(opcodes[2]),
        binaryRow<W, &integerPick<W, &numerics::gtU<Box>, Unsigned>>(opcodes[3]),
    });
}

/// The saturating rows of one lane width: add_sat_s, add_sat_u, sub_sat_s, sub_sat_u.
template <unsigned W>
constexpr auto saturatingRows(std::array<Opcode, 4> const& opcodes)
{
    return std::to_array<LanewiseRow>({
        binaryRow<W, &saturating<W, &numerics::add<I32>, Signed>>(opcodes[0]),
        binaryRow<W, &saturating<W, &numerics::add<I32>, Unsigned>>(opcodes[1]),
        binaryRow<W, &saturating<W, &numerics::sub<I32>, Signed>>(opcodes[2]),
        binaryRow<W, &saturating<W, &numerics::sub<I32>, Unsigned>>(opcodes[3]),
    });
}

/// The float rows of one shape: abs, neg, sqrt, ceil, floor, trunc, nearest; add, sub, mul, div,
/// min, max, pmin, pmax, relaxed_min, relaxed_max; eq, ne, lt, gt, le, ge.
template <typename Box>
constexpr auto floatRows(std::array<Opcode, 7> const& unary,
                         std::array<Opcode, 10> const& binary,
                         std::array<Opcode, 6> const& compare)
{
    constexpr auto W = unsigned { std::numeric_limits<numerics::FloatBits<Box>>::digits };
    return std::to_array<LanewiseRow>({
        unaryRow<W, W, &scalarUnary<Box, &numerics::fabs<Box>>>(unary[0]),
        unaryRow<W, W, &scalarUnary<Box, &numerics::fneg<Box>>>(unary[1]),
        unaryRow<W, W, &scalarUnary<Box, &numerics::sqrt<Box>>>(unary[2]),
        unaryRow<W, W, &scalarUnary<Box, &numerics::ceil<Box>>>(unary[3]),
        unaryRow<W, W, &scalarUnary<Box, &numerics::floor<Box>>>(unary[4]),
        unaryRow<W, W, &scalarUnary<Box, &numerics::trunc<Box>>>(unary[5]),
        unaryRow<W, W, &scalarUnary<Box, &numerics::nearest<Box>>>(unary[6]),
        binaryRow<W, &floatBinary<Box, &numerics::fadd<Box>>>(binary[0]),
        binaryRow<W, &floatBinary<Box, &numerics::fsub<Box>>>(binary[1]),
        binaryRow<W, &floatBinary<Box, &numerics::fmul<Box>>>(binary[2]),
        binaryRow<W, &floatBinary<Box, &numerics::fdiv<Box>>>(binary[3]),
        binaryRow<W, &floatBinary<Box, &numerics::fmin<Box>>>(binary[4]),
        binaryRow<W, &floatBinary<Box, &numerics::fmax<Box>>>(binary[5]),
        binaryRow<W, &pseudoMinimum<Box>>(binary[6]),
        binaryRow<W, &pseudoMaximum<Box>>(binary[7]),
        binaryRow<W, &floatBinary<Box, &numerics::fmin<Box>>>(binary[8]),
        binaryRow<W, &floatBinary<Box, &numerics::fmax<Box>>>(binary[9]),
        binaryRow<W, &floatCompare<Box, &numerics::feq<Box>>>(compare[0]),
        binaryRow<W, &floatCompare<Box, &numerics::fne<Box>>>(compare[1]),
        binaryRow<W, &floatCompare<Box, &numerics::flt<Box>>>(compare[2]),
        binaryRow<W, &floatCompare<Box, &numerics::fgt<Box>>>(compare[3]),
        binaryRow<W, &floatCompare<Box, &numerics::fle<Box>>>(compare[4]),
        binaryRow<W, &floatCompare<Box, &numerics::fge<Box>>>(compare[5]),
    });
}

/// The rows of @p parts, one after the other.
template <std::size_t... Sizes>
constexpr auto joined(std::array<LanewiseRow, Sizes> const&... parts)
{
    auto rows = std::array<LanewiseRow, (Sizes + ...)> {};
    auto out = rows.begin();
    ((out = std::ranges::copy(parts, out).out), ...);
    return rows;
}

// clang-format off
/// Every lane-wise kernel row, with the scalar reference of a lane.
constexpr auto LanewiseRows = joined(
    std::to_array<LanewiseRow>({
        unaryRow<64, 64, &bitwiseNot>(Opcode::V128Not),
        binaryRow<64, &integerBinary<64, &numerics::bitAnd<I64>>>(Opcode::V128And),
        binaryRow<64, &bitwiseAndNot>(Opcode::V128Andnot),
        binaryRow<64, &integerBinary<64, &numerics::bitOr<I64>>>(Opcode::V128Or),
        binaryRow<64, &integerBinary<64, &numerics::bitXor<I64>>>(Opcode::V128Xor),
        binaryRow<16, &integerBinary<16, &numerics::mul<I32>>>(Opcode::I16x8Mul),
        binaryRow<32, &integerBinary<32, &numerics::mul<I32>>>(Opcode::I32x4Mul),
        binaryRow<64, &integerBinary<64, &numerics::mul<I64>>>(Opcode::I64x2Mul),
        unaryRow<8, 8, &integerPopcnt<8>>(Opcode::I8x16Popcnt),
        // Pairwise sums and dot products: lane-wise in the wide lane, of the narrow lanes within it
        binaryRow<32, &pairDot<16, 32>>(Opcode::I32x4DotI16x8S),
        binaryRow<16, &pairDot<8, 16>>(Opcode::I16x8RelaxedDotI8x16I7x16S),
        unaryRow<16, 16, &pairSum<8, 16, Signed>>(Opcode::I16x8ExtaddPairwiseI8x16S),
        unaryRow<16, 16, &pairSum<8, 16, Unsigned>>(Opcode::I16x8ExtaddPairwiseI8x16U),
        unaryRow<32, 32, &pairSum<16, 32, Signed>>(Opcode::I32x4ExtaddPairwiseI16x8S),
        unaryRow<32, 32, &pairSum<16, 32, Unsigned>>(Opcode::I32x4ExtaddPairwiseI16x8U),
        binaryRow<8, &averageRounded<8>>(Opcode::I8x16AvgrU),
        binaryRow<16, &averageRounded<16>>(Opcode::I16x8AvgrU),
        binaryRow<16, &q15MulRound>(Opcode::I16x8Q15mulrSatS),
        binaryRow<16, &q15MulRound>(Opcode::I16x8RelaxedQ15mulrS),
        binaryRow<64, &integerBinary<64, &numerics::add<I64>>>(Opcode::I64x2Add),
        binaryRow<64, &integerBinary<64, &numerics::sub<I64>>>(Opcode::I64x2Sub),
        unaryRow<64, 64, &integerNeg<64>>(Opcode::I64x2Neg),
        unaryRow<64, 64, &integerAbs<64>>(Opcode::I64x2Abs),
        binaryRow<64, &integerCompare<64, &numerics::eq<I64>, Unsigned>>(Opcode::I64x2Eq),
        binaryRow<64, &integerCompare<64, &numerics::ne<I64>, Unsigned>>(Opcode::I64x2Ne),
        binaryRow<64, &integerCompare<64, &numerics::ltS<I64>, Signed>>(Opcode::I64x2LtS),
        binaryRow<64, &integerCompare<64, &numerics::gtS<I64>, Signed>>(Opcode::I64x2GtS),
        binaryRow<64, &integerCompare<64, &numerics::leS<I64>, Signed>>(Opcode::I64x2LeS),
        binaryRow<64, &integerCompare<64, &numerics::geS<I64>, Signed>>(Opcode::I64x2GeS),
        // Conversions of one lane width
        unaryRow<32, 32, &scalarUnary<F32, &numerics::truncSaturating<I32, int32_t, F32>>>(Opcode::I32x4TruncSatF32x4S),
        unaryRow<32, 32, &scalarUnary<F32, &numerics::truncSaturating<I32, uint32_t, F32>>>(Opcode::I32x4TruncSatF32x4U),
        unaryRow<32, 32, &scalarUnary<F32, &numerics::truncSaturating<I32, int32_t, F32>>>(Opcode::I32x4RelaxedTruncF32x4S),
        unaryRow<32, 32, &scalarUnary<F32, &numerics::truncSaturating<I32, uint32_t, F32>>>(Opcode::I32x4RelaxedTruncF32x4U),
        unaryRow<32, 32, &scalarUnary<I32, &numerics::convert<F32, int32_t, I32>>>(Opcode::F32x4ConvertI32x4S),
        unaryRow<32, 32, &scalarUnary<I32, &numerics::convert<F32, uint32_t, I32>>>(Opcode::F32x4ConvertI32x4U),
        // Conversions between lane widths: the lower lanes
        unaryRow<64, 32, &scalarUnary<F64, &numerics::truncSaturating<I32, int32_t, F64>>>(Opcode::I32x4TruncSatF64x2SZero),
        unaryRow<64, 32, &scalarUnary<F64, &numerics::truncSaturating<I32, uint32_t, F64>>>(Opcode::I32x4TruncSatF64x2UZero),
        unaryRow<64, 32, &scalarUnary<F64, &numerics::truncSaturating<I32, int32_t, F64>>>(Opcode::I32x4RelaxedTruncF64x2SZero),
        unaryRow<64, 32, &scalarUnary<F64, &numerics::truncSaturating<I32, uint32_t, F64>>>(Opcode::I32x4RelaxedTruncF64x2UZero),
        unaryRow<32, 64, &scalarUnary<I32, &numerics::convert<F64, int32_t, I32>>>(Opcode::F64x2ConvertLowI32x4S),
        unaryRow<32, 64, &scalarUnary<I32, &numerics::convert<F64, uint32_t, I32>>>(Opcode::F64x2ConvertLowI32x4U),
        unaryRow<64, 32, &scalarUnary<F64, &numerics::demote>>(Opcode::F32x4DemoteF64x2Zero),
        unaryRow<32, 64, &scalarUnary<F32, &numerics::promote>>(Opcode::F64x2PromoteLowF32x4),
    }),
    integerRows<8>({ Opcode::I8x16Add, Opcode::I8x16Sub, Opcode::I8x16Neg, Opcode::I8x16Abs },
                   { Opcode::I8x16Eq, Opcode::I8x16Ne, Opcode::I8x16LtS, Opcode::I8x16LtU, Opcode::I8x16GtS,
                     Opcode::I8x16GtU, Opcode::I8x16LeS, Opcode::I8x16LeU, Opcode::I8x16GeS, Opcode::I8x16GeU }),
    integerRows<16>({ Opcode::I16x8Add, Opcode::I16x8Sub, Opcode::I16x8Neg, Opcode::I16x8Abs },
                    { Opcode::I16x8Eq, Opcode::I16x8Ne, Opcode::I16x8LtS, Opcode::I16x8LtU, Opcode::I16x8GtS,
                      Opcode::I16x8GtU, Opcode::I16x8LeS, Opcode::I16x8LeU, Opcode::I16x8GeS, Opcode::I16x8GeU }),
    integerRows<32>({ Opcode::I32x4Add, Opcode::I32x4Sub, Opcode::I32x4Neg, Opcode::I32x4Abs },
                    { Opcode::I32x4Eq, Opcode::I32x4Ne, Opcode::I32x4LtS, Opcode::I32x4LtU, Opcode::I32x4GtS,
                      Opcode::I32x4GtU, Opcode::I32x4LeS, Opcode::I32x4LeU, Opcode::I32x4GeS, Opcode::I32x4GeU }),
    shiftRows<8>({ Opcode::I8x16Shl, Opcode::I8x16ShrS, Opcode::I8x16ShrU }),
    shiftRows<16>({ Opcode::I16x8Shl, Opcode::I16x8ShrS, Opcode::I16x8ShrU }),
    shiftRows<32>({ Opcode::I32x4Shl, Opcode::I32x4ShrS, Opcode::I32x4ShrU }),
    shiftRows<64>({ Opcode::I64x2Shl, Opcode::I64x2ShrS, Opcode::I64x2ShrU }),
    pickRows<8>({ Opcode::I8x16MinS, Opcode::I8x16MinU, Opcode::I8x16MaxS, Opcode::I8x16MaxU }),
    pickRows<16>({ Opcode::I16x8MinS, Opcode::I16x8MinU, Opcode::I16x8MaxS, Opcode::I16x8MaxU }),
    pickRows<32>({ Opcode::I32x4MinS, Opcode::I32x4MinU, Opcode::I32x4MaxS, Opcode::I32x4MaxU }),
    saturatingRows<8>({ Opcode::I8x16AddSatS, Opcode::I8x16AddSatU, Opcode::I8x16SubSatS, Opcode::I8x16SubSatU }),
    saturatingRows<16>({ Opcode::I16x8AddSatS, Opcode::I16x8AddSatU, Opcode::I16x8SubSatS, Opcode::I16x8SubSatU }),
    floatRows<F32>({ Opcode::F32x4Abs, Opcode::F32x4Neg, Opcode::F32x4Sqrt, Opcode::F32x4Ceil, Opcode::F32x4Floor,
                     Opcode::F32x4Trunc, Opcode::F32x4Nearest },
                   { Opcode::F32x4Add, Opcode::F32x4Sub, Opcode::F32x4Mul, Opcode::F32x4Div, Opcode::F32x4Min,
                     Opcode::F32x4Max, Opcode::F32x4Pmin, Opcode::F32x4Pmax, Opcode::F32x4RelaxedMin,
                     Opcode::F32x4RelaxedMax },
                   { Opcode::F32x4Eq, Opcode::F32x4Ne, Opcode::F32x4Lt, Opcode::F32x4Gt, Opcode::F32x4Le,
                     Opcode::F32x4Ge }),
    floatRows<F64>({ Opcode::F64x2Abs, Opcode::F64x2Neg, Opcode::F64x2Sqrt, Opcode::F64x2Ceil, Opcode::F64x2Floor,
                     Opcode::F64x2Trunc, Opcode::F64x2Nearest },
                   { Opcode::F64x2Add, Opcode::F64x2Sub, Opcode::F64x2Mul, Opcode::F64x2Div, Opcode::F64x2Min,
                     Opcode::F64x2Max, Opcode::F64x2Pmin, Opcode::F64x2Pmax, Opcode::F64x2RelaxedMin,
                     Opcode::F64x2RelaxedMax },
                   { Opcode::F64x2Eq, Opcode::F64x2Ne, Opcode::F64x2Lt, Opcode::F64x2Gt, Opcode::F64x2Le,
                     Opcode::F64x2Ge }));

/// The kernel rows whose result lane is not a function of the same operand lane alone: they move
/// lanes between widths or positions, reduce them, or take a third operand. reference/Simd_test.cpp
/// checks each against worked values (Simd.extend, .extmul, .narrowSaturates,
/// .swizzleOutOfRangeIsZero, .bitselect, .allTrue, .bitmask, .bitwise, and the Relaxed.* cases).
constexpr auto NotLanewise = std::to_array<Opcode>({
    Opcode::I16x8ExtendLowI8x16S, Opcode::I16x8ExtendLowI8x16U, Opcode::I16x8ExtendHighI8x16S,
    Opcode::I16x8ExtendHighI8x16U, Opcode::I32x4ExtendLowI16x8S, Opcode::I32x4ExtendLowI16x8U,
    Opcode::I32x4ExtendHighI16x8S, Opcode::I32x4ExtendHighI16x8U, Opcode::I64x2ExtendLowI32x4S,
    Opcode::I64x2ExtendLowI32x4U, Opcode::I64x2ExtendHighI32x4S, Opcode::I64x2ExtendHighI32x4U,
    Opcode::I8x16Swizzle, Opcode::I8x16RelaxedSwizzle,
    Opcode::I16x8ExtmulLowI8x16S, Opcode::I16x8ExtmulLowI8x16U, Opcode::I16x8ExtmulHighI8x16S,
    Opcode::I16x8ExtmulHighI8x16U, Opcode::I32x4ExtmulLowI16x8S, Opcode::I32x4ExtmulLowI16x8U,
    Opcode::I32x4ExtmulHighI16x8S, Opcode::I32x4ExtmulHighI16x8U, Opcode::I64x2ExtmulLowI32x4S,
    Opcode::I64x2ExtmulLowI32x4U, Opcode::I64x2ExtmulHighI32x4S, Opcode::I64x2ExtmulHighI32x4U,
    Opcode::I8x16NarrowI16x8S, Opcode::I8x16NarrowI16x8U, Opcode::I16x8NarrowI32x4S, Opcode::I16x8NarrowI32x4U,
    Opcode::V128Bitselect, Opcode::I8x16RelaxedLaneselect, Opcode::I16x8RelaxedLaneselect,
    Opcode::I32x4RelaxedLaneselect, Opcode::I64x2RelaxedLaneselect,
    Opcode::F32x4RelaxedMadd, Opcode::F32x4RelaxedNmadd, Opcode::F64x2RelaxedMadd, Opcode::F64x2RelaxedNmadd,
    Opcode::I32x4RelaxedDotI8x16I7x16AddS,
    Opcode::V128AnyTrue, Opcode::I8x16AllTrue, Opcode::I16x8AllTrue, Opcode::I32x4AllTrue, Opcode::I64x2AllTrue,
    Opcode::I8x16Bitmask, Opcode::I16x8Bitmask, Opcode::I32x4Bitmask, Opcode::I64x2Bitmask,
});
// clang-format on

// --- Vectors ------------------------------------------------------------------------------

/// A fixed-seed xorshift64 generator, so every run checks the same vectors.
class Generator
{
  public:
    uint64_t next() noexcept
    {
        _state ^= _state << 13U;
        _state ^= _state >> 7U;
        _state ^= _state << 17U;
        return _state;
    }

  private:
    uint64_t _state = 0x9E37'79B9'7F4A'7C15;
};

/// Lane values that random bits rarely give: zeros, extremes, infinities, NaNs (canonical, signalling,
/// with payloads), halves and integral floats, each of both widths that floats have.
constexpr auto SpecialLanes = std::to_array<uint64_t>({
    0,
    1,
    ~uint64_t { 0 },
    0x8000'0000'0000'0000,
    0x7FFF'FFFF'FFFF'FFFF,
    0x8000'0000,
    0x7FFF'FFFF,
    0x8000,
    0x80,
    0x7F80'0000,                            // f32 +inf
    0xFF80'0000,                            // f32 -inf
    0x7FC0'0000,                            // f32 canonical NaN
    0xFF80'0001,                            // f32 signalling NaN with a payload
    0x3F00'0000,                            // f32 0.5
    0x4020'0000,                            // f32 2.5
    0x4F00'0000,                            // f32 2^31
    0x7FF0'0000'0000'0000,                  // f64 +inf
    0x7FF8'0000'0000'0000,                  // f64 canonical NaN
    0xFFF0'0000'0000'00FF,                  // f64 signalling NaN with a payload
    std::bit_cast<uint64_t>(-0.0),          // f64 -0
    std::bit_cast<uint64_t>(2.5),           // f64 2.5
    std::bit_cast<uint64_t>(-2147483648.5), // f64 just below i32's range
    std::bit_cast<uint64_t>(4294967295.0),  // f64 u32's maximum
});

/// A vector of @p laneBits-wide lanes, each random or, one in four, a special value's low bits.
V128 vectorOf(Generator& generator, unsigned laneBits)
{
    auto vector = V128 {};
    for (auto const lane: std::views::iota(0U, 128 / laneBits))
    {
        auto const choice = generator.next();
        auto const value =
            (choice & 3U) == 0 ? SpecialLanes.at((choice >> 2U) % SpecialLanes.size()) : generator.next();
        auto const mask = laneBits == 64 ? ~uint64_t { 0 } : (uint64_t { 1 } << laneBits) - 1;
        auto const shift = (lane * laneBits) % 64;
        vector.lanes.at(lane * laneBits / 64) |= (value & mask) << shift;
    }
    return vector;
}

/// Lane @p index of @p vector, @p laneBits wide.
uint64_t laneAt(V128 const& vector, unsigned laneBits, unsigned index)
{
    auto const mask = laneBits == 64 ? ~uint64_t { 0 } : (uint64_t { 1 } << laneBits) - 1;
    return (vector.lanes.at(index * laneBits / 64) >> ((index * laneBits) % 64)) & mask;
}

constexpr auto VectorsPerRow = 64;

} // namespace

TEST_CASE("SimdKernels.everyShapedOpcodeHasAKernel")
{
    for (auto const& row: OpcodeTable)
    {
        INFO(row.text);
        auto const shape = simdShape(row.opcode);
        CHECK((shape == SimdShape::Unary) == (numerics::simdUnaryKernel(row.opcode) != nullptr));
        CHECK((shape == SimdShape::Binary) == (numerics::simdBinaryKernel(row.opcode) != nullptr));
        CHECK((shape == SimdShape::Ternary) == (numerics::simdTernaryKernel(row.opcode) != nullptr));
        CHECK((shape == SimdShape::Test) == (numerics::simdTestKernel(row.opcode) != nullptr));
        CHECK((shape == SimdShape::Shift) == (numerics::simdShiftKernel(row.opcode) != nullptr));

        // Every SIMD operator has a shape: all 0xFD-prefixed rows but the memory accesses and the two
        // that take a v128 immediate, which each executor runs itself.
        auto const isOperator = row.prefix == 0xFD && row.signature != OpcodeSignature::Special
                                && row.opcode != Opcode::V128Const && row.opcode != Opcode::I8x16Shuffle;
        CHECK((shape != SimdShape::None) == isOperator);
    }
}

TEST_CASE("SimdKernels.agreeWithTheScalarKernelPerLane")
{
    auto generator = Generator {};
    for (auto const& row: LanewiseRows)
    {
        INFO(isoline::binary::opcodeInfo(row.opcode).text);
        auto const operandLanes = 128 / row.operandBits;
        auto const resultLanes = 128 / row.resultBits;
        auto const convertedLanes = std::min(operandLanes, resultLanes);
        for ([[maybe_unused]] auto const _: std::views::iota(0, VectorsPerRow))
        {
            auto const lhs = vectorOf(generator, row.operandBits);
            auto const rhs = vectorOf(generator, row.operandBits);
            auto const count = static_cast<uint32_t>(generator.next());
            auto result = V128 {};
            if (row.unary != nullptr)
            {
                auto const kernel = numerics::simdUnaryKernel(row.opcode);
                REQUIRE(kernel != nullptr);
                result = kernel(lhs);
            }
            else if (row.binary != nullptr)
            {
                auto const kernel = numerics::simdBinaryKernel(row.opcode);
                REQUIRE(kernel != nullptr);
                result = kernel(lhs, rhs);
            }
            else
            {
                auto const kernel = numerics::simdShiftKernel(row.opcode);
                REQUIRE(kernel != nullptr);
                result = kernel(lhs, count);
            }
            for (auto const lane: std::views::iota(0U, resultLanes))
            {
                INFO("lane " << lane << " of " << std::hex << lhs.lanes[1] << ':' << lhs.lanes[0] << ", "
                             << rhs.lanes[1] << ':' << rhs.lanes[0] << ", count " << std::dec << count);
                auto const expected = [&]() -> uint64_t {
                    if (lane >= convertedLanes)
                        return 0;
                    auto const a = laneAt(lhs, row.operandBits, lane);
                    if (row.unary != nullptr)
                        return row.unary(a);
                    if (row.binary != nullptr)
                        return row.binary(a, laneAt(rhs, row.operandBits, lane));
                    return row.shift(a, count);
                }();
                CHECK(laneAt(result, row.resultBits, lane) == expected);
            }
        }
    }

    SECTION("every kernel row is lane-wise and checked here, or listed as moving lanes")
    {
        for (auto const& row: OpcodeTable)
        {
            auto const hasKernel = numerics::simdUnaryKernel(row.opcode) != nullptr
                                   || numerics::simdBinaryKernel(row.opcode) != nullptr
                                   || numerics::simdTernaryKernel(row.opcode) != nullptr
                                   || numerics::simdTestKernel(row.opcode) != nullptr
                                   || numerics::simdShiftKernel(row.opcode) != nullptr;
            auto const isLanewise =
                std::ranges::find(LanewiseRows, row.opcode, &LanewiseRow::opcode) != LanewiseRows.end();
            auto const isListed = std::ranges::find(NotLanewise, row.opcode) != NotLanewise.end();
            INFO(row.text);
            CHECK(hasKernel == (isLanewise || isListed));
            CHECK_FALSE((isLanewise && isListed));
        }
    }
}

TEST_CASE("SimdKernels.mapLanesAppliesTheScalarKernelToEachLane")
{
    // The template the fast interpreter's dedicated rows inline: no Value, no expected, one lane type.
    auto const lhs = V128 { .lanes = { 0x4000'0000'3F80'0000, 0x7FC0'0000'C000'0000 } }; // 1, 2, -2, NaN
    auto const rhs = V128 { .lanes = { 0x3F80'0000'3F80'0000, 0x3F80'0000'3F80'0000 } }; // 1, 1, 1, 1
    auto const sum = numerics::mapLanes<F32, &numerics::fadd<F32>>(lhs, rhs);
    CHECK(sum.lanes[0] == 0x4040'0000'4000'0000); // 3, 2
    CHECK(sum.lanes[1] == 0x7FC0'0000'BF80'0000); // NaN, -1

    auto const wrapped = numerics::mapLanes<uint8_t, &numerics::laneAdd<uint8_t>>(
        V128 { .lanes = { 0xFF, 0 } }, V128 { .lanes = { 0x02, 0 } });
    CHECK(wrapped.lanes[0] == 0x01);

    auto const isLess = numerics::mapLanes<F64, &numerics::flt<F64>>(
        V128 { .lanes = { std::bit_cast<uint64_t>(1.0), std::bit_cast<uint64_t>(3.0) } },
        V128 { .lanes = { std::bit_cast<uint64_t>(2.0), std::bit_cast<uint64_t>(2.0) } });
    CHECK(isLess.lanes[0] == ~uint64_t { 0 });
    CHECK(isLess.lanes[1] == 0);
}
