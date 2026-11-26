// SPDX-License-Identifier: Apache-2.0
#include <isoline/numerics/SimdKernels.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <variant>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/numerics/Float.hpp>
#include <isoline/numerics/Integer.hpp>
#include <isoline/numerics/SimdLanes.hpp>
#include <isoline/numerics/SimdShape.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::numerics
{

namespace
{
    using binary::Extension;
    using binary::Opcode;
    using detail::laneAt;
    using detail::lanewise;
    using detail::putLane;
    using runtime::F32;
    using runtime::F64;
    using runtime::I32;
    using runtime::I64;
    using runtime::V128;

    /// An integer operator reads its lanes as signed (the `_s` forms) ...
    constexpr auto Signed = Extension::Sign;
    /// ... or as unsigned (the `_u` forms, and those for which the sign does not matter).
    constexpr auto Unsigned = Extension::Zero;

    // --- Bitwise operators --------------------------------------------------------------------

    [[nodiscard]] constexpr V128 bitNot(V128 operand) noexcept
    {
        return V128 { .lanes = { ~operand.lanes[0], ~operand.lanes[1] } };
    }

    [[nodiscard]] constexpr V128 bitAndNot(V128 lhs, V128 rhs) noexcept
    {
        return mapLanes<I64, &bitAnd<I64>>(lhs, bitNot(rhs));
    }

    /// v128.bitselect: each bit from @p first where @p mask's is set, else from @p second.
    [[nodiscard]] constexpr V128 bitSelect(V128 first, V128 second, V128 mask) noexcept
    {
        return mapLanes<I64, &bitOr<I64>>(mapLanes<I64, &bitAnd<I64>>(first, mask), bitAndNot(second, mask));
    }

    [[nodiscard]] constexpr I32 anyTrue(V128 operand) noexcept
    {
        return I32 { (operand.lanes[0] | operand.lanes[1]) != 0 ? 1U : 0U };
    }

    /// i8x16.swizzle: byte i is the byte of @p bytes that byte i of @p indices names, or 0 when
    /// that index is 16 or more.
    [[nodiscard]] constexpr V128 swizzle(V128 bytes, V128 indices) noexcept
    {
        return lanewise<uint8_t>(
            [&bytes](uint8_t index) {
                return index < LaneCount<uint8_t> ? laneAt<uint8_t>(bytes, index) : uint8_t { 0 };
            },
            indices);
    }

    // --- Lane tests ---------------------------------------------------------------------------

    /// 1 when every lane is non-zero, else 0.
    template <std::unsigned_integral Lane>
    [[nodiscard]] constexpr I32 allTrue(V128 operand) noexcept
    {
        auto const lanes = std::views::iota(0U, LaneCount<Lane>);
        auto const isSet = [&operand](unsigned index) {
            return laneAt<Lane>(operand, index) != 0;
        };
        return I32 { std::ranges::all_of(lanes, isSet) ? 1U : 0U };
    }

    /// Bit i of the result is the top bit of lane i.
    template <std::unsigned_integral Lane>
    [[nodiscard]] constexpr I32 bitmask(V128 operand) noexcept
    {
        auto mask = uint32_t { 0 };
        for (auto const index: std::views::iota(0U, LaneCount<Lane>))
            mask |= static_cast<uint32_t>(laneAt<Lane>(operand, index) >> (LaneBits<Lane> - 1)) << index;
        return I32 { mask };
    }

    // --- Widening and narrowing ---------------------------------------------------------------

    /// Which half of a vector's narrow lanes an operator widens.
    enum class Half : uint8_t
    {
        Low,  ///< Lanes 0 to half the count, exclusive.
        High, ///< The other half.
    };

    /// The first narrow lane of @p Which half of a vector of @p Narrow lanes.
    template <std::unsigned_integral Narrow, Half Which>
    inline constexpr auto HalfStart = Which == Half::Low ? 0U : LaneCount<Narrow> / 2;

    /// extend_low/extend_high: @p Which half of the @p Narrow lanes, each widened to @p Wide.
    template <std::unsigned_integral Narrow, std::unsigned_integral Wide, Half Which, Extension Widening>
    [[nodiscard]] constexpr V128 extendLanes(V128 operand) noexcept
    {
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, LaneCount<Wide>))
        {
            auto const lane = laneAt<Narrow>(operand, HalfStart<Narrow, Which> + index);
            putLane<Wide>(result, index, widened<Wide, Widening>(lane));
        }
        return result;
    }

    /// extmul_low/extmul_high: the products of @p Which half of the @p Narrow lanes, widened to
    /// @p Wide, which holds each product exactly.
    template <std::unsigned_integral Narrow, std::unsigned_integral Wide, Half Which, Extension Widening>
    [[nodiscard]] constexpr V128 extendMultiply(V128 lhs, V128 rhs) noexcept
    {
        auto const wideLhs = extendLanes<Narrow, Wide, Which, Widening>(lhs);
        auto const wideRhs = extendLanes<Narrow, Wide, Which, Widening>(rhs);
        return mapLanes<Wide, &laneMul<Wide>>(wideLhs, wideRhs);
    }

    // Pairwise sums and dot products: wide lane i is a function of the narrow lanes within it,
    // so they are lane-wise kernels of the wide lane type, whose scalar operation splits each wide
    // lane into its narrow parts.

    /// Narrow lane @p part (0 the lowest bits) of the wide lane @p lane, read per @p Reading and
    /// widened to int64_t.
    template <std::unsigned_integral Narrow, Extension Reading, std::unsigned_integral Wide>
    [[nodiscard]] constexpr int64_t narrowPart(Wide lane, unsigned part) noexcept
    {
        auto const shift = part * LaneBits<Narrow>;
        // The top part is the whole lane, read per Reading, shifted down: one shift, no truncation.
        if (shift + LaneBits<Narrow> == LaneBits<Wide>)
            return int64_t { interpreted<Reading>(lane) >> shift };
        return int64_t { interpreted<Reading>(static_cast<Narrow>(lane >> shift)) };
    }

    /// The sum of the products of the narrow parts of @p lhs and @p rhs, read per @p Reading; at
    /// most 4 * 128 * 128 for bytes and 2 * 2^30 for i16s, so an int64_t holds it exactly.
    template <std::unsigned_integral Narrow, std::unsigned_integral Wide>
    [[nodiscard]] constexpr int64_t signedDot(Wide lhs, Wide rhs) noexcept
    {
        auto sum = int64_t { 0 };
        for (auto const part: std::views::iota(0U, unsigned { sizeof(Wide) / sizeof(Narrow) }))
            sum += narrowPart<Narrow, Signed>(lhs, part) * narrowPart<Narrow, Signed>(rhs, part);
        return sum;
    }

    /// extadd_pairwise of one wide lane: the sum of its two narrow parts, each read per @p Widening.
    template <std::unsigned_integral Narrow, std::unsigned_integral Wide, Extension Widening>
    [[nodiscard]] constexpr Wide pairwiseSum(Wide lane) noexcept
    {
        return static_cast<Wide>(narrowPart<Narrow, Widening>(lane, 0)
                                 + narrowPart<Narrow, Widening>(lane, 1));
    }

    /// extadd_pairwise: lane i is the sum of narrow lanes 2i and 2i + 1, each widened to @p Wide.
    template <std::unsigned_integral Narrow, std::unsigned_integral Wide, Extension Widening>
    [[nodiscard]] constexpr V128 extendAddPairwise(V128 operand) noexcept
    {
        return mapLanes<Wide, &pairwiseSum<Narrow, Wide, Widening>>(operand);
    }

    /// i32x4.dot_i16x8_s of one lane: the sum of the signed products of its two i16 parts, wrapping.
    [[nodiscard]] constexpr uint32_t dotLane(uint32_t lhs, uint32_t rhs) noexcept
    {
        return static_cast<uint32_t>(signedDot<uint16_t>(lhs, rhs));
    }

    /// narrow: the @p Wide lanes of @p lhs, then of @p rhs, each read as signed and saturated to a
    /// @p Narrow lane read per @p Reading.
    template <std::unsigned_integral Wide, std::unsigned_integral Narrow, Extension Reading>
    [[nodiscard]] constexpr V128 narrowLanes(V128 lhs, V128 rhs) noexcept
    {
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, LaneCount<Narrow>))
        {
            auto const& source = index < LaneCount<Wide> ? lhs : rhs;
            auto const lane = interpreted<Signed>(laneAt<Wide>(source, index % LaneCount<Wide>));
            putLane<Narrow>(result, index, saturated<Narrow, Reading>(lane));
        }
        return result;
    }

    // --- Floating-point operators and conversions ---------------------------------------------
    //
    // Each lane goes through the scalar kernel of Float.hpp, on its bits: that is where NaN
    // canonicalisation, rounding on bits and the exact conversions live, so a vector lane computes
    // what its scalar instruction does by construction.

    /// pmin: the spec's select-based definition on lt, b < a ? b : a; the result is an
    /// operand's bits, a NaN's included, never a canonical NaN.
    template <typename Box>
    [[nodiscard]] constexpr Box pseudoMinimum(Box a, Box b) noexcept
    {
        return flt(b, a) ? b : a;
    }

    /// pmax: a < b ? b : a, an operand's bits.
    template <typename Box>
    [[nodiscard]] constexpr Box pseudoMaximum(Box a, Box b) noexcept
    {
        return flt(a, b) ? b : a;
    }

    /// The scalar conversion @p Operation on each lane of @p operand read as a @p From, giving a
    /// lane of @p To, where the two lane widths differ: the lower lanes are converted, as many as
    /// the wider shape has, and the rest of the result is zero. promote_low and convert_low read
    /// lanes 0 and 1, demote_zero and trunc_sat_zero write them.
    template <SimdLane From, SimdLane To, auto Operation>
    [[nodiscard]] V128 convertLowLanes(V128 operand) noexcept
    {
        using FromBits = LaneBitsOf<From>;
        using ToBits = LaneBitsOf<To>;
        constexpr auto Count = std::min(LaneCount<FromBits>, LaneCount<ToBits>);
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, Count))
        {
            auto const lane = From { laneAt<FromBits>(operand, index) };
            putLane<ToBits>(result, index, Operation(lane).bits);
        }
        return result;
    }

    // --- Relaxed SIMD: the Wasmtime-deterministic profile (SimdKernels.hpp) --------------------
    //
    // The relaxed operators that are an existing operator in that profile (swizzle, trunc_sat,
    // bitselect, min, max, q15mulr_sat_s) have that operator's kernel in the table; only these
    // three have a definition of their own.

    /// Whether relaxed_madd or relaxed_nmadd adds the product or its negation.
    enum class ProductSign : uint8_t
    {
        Positive, ///< relaxed_madd: a * b + c.
        Negated,  ///< relaxed_nmadd: -(a * b) + c.
    };

    /// relaxed_madd and relaxed_nmadd of one lane: the product and the sum rounded once, as
    /// std::fma rounds them exactly on every host (in the default floating-point environment,
    /// which every guest entry establishes, as for add and mul). A NaN result is the canonical NaN.
    template <typename Box, ProductSign Sign>
    [[nodiscard]] Box fusedMultiplyAdd(Box a, Box b, Box c) noexcept
    {
        auto const factor = Sign == ProductSign::Negated ? toHost(fneg(a)) : toHost(a);
        return arithmeticResult<Box>(std::fma(factor, toHost(b), toHost(c)));
    }

    /// i16x8.relaxed_dot_i8x16_i7x16_s of one lane: the sum of the signed products of its two
    /// bytes, wrapping at 16 bits (two products of -128 sum to 0x8000).
    [[nodiscard]] constexpr uint16_t relaxedDotLane(uint16_t lhs, uint16_t rhs) noexcept
    {
        return static_cast<uint16_t>(signedDot<uint8_t>(lhs, rhs));
    }

    /// i32x4.relaxed_dot_i8x16_i7x16_add_s of one lane: the exact sum of the signed products of its
    /// four bytes, with no 16-bit intermediate, plus @p addend, wrapping at 32 bits.
    [[nodiscard]] constexpr uint32_t relaxedDotAddLane(uint32_t lhs, uint32_t rhs, uint32_t addend) noexcept
    {
        return laneAdd(static_cast<uint32_t>(signedDot<uint8_t>(lhs, rhs)), addend);
    }

    // --- The table ----------------------------------------------------------------------------

    /// A lane operation's row: the instruction takes a scalar or a lane index immediate, which
    /// each executor handles with a dedicated row of its own, so the table holds its shape alone.
    template <SimdShape Shape>
    struct LaneOperation
    {
    };

    /// A SIMD operator's kernel. Its alternatives are in SimdShape's order, so the alternative a
    /// row holds is its shape: the one fact is stated once.
    using Kernel = std::variant<std::monostate,
                                SimdUnaryKernel,
                                SimdBinaryKernel,
                                SimdTernaryKernel,
                                SimdTestKernel,
                                SimdShiftKernel,
                                LaneOperation<SimdShape::Splat>,
                                LaneOperation<SimdShape::ExtractLane>,
                                LaneOperation<SimdShape::ReplaceLane>>;

    static_assert(std::variant_size_v<Kernel> == static_cast<std::size_t>(SimdShape::ReplaceLane) + 1,
                  "Kernel holds one alternative per SimdShape, in its order");

    /// One SIMD operator: the instruction and its kernel.
    struct SimdRow
    {
        Opcode opcode; ///< The instruction.
        Kernel kernel; ///< What evaluates it; its alternative is the instruction's shape.
    };

    using Unary = SimdUnaryKernel;
    using Binary = SimdBinaryKernel;
    using Ternary = SimdTernaryKernel;
    using Test = SimdTestKernel;
    using Shift = SimdShiftKernel;
    using Splat = LaneOperation<SimdShape::Splat>;
    using Extract = LaneOperation<SimdShape::ExtractLane>;
    using Replace = LaneOperation<SimdShape::ReplaceLane>;

    template <std::unsigned_integral Lane, typename Relation, Extension Reading>
    constexpr auto IntegerCompare = Binary { &mapLanes<Lane, &laneCompare<Lane, Relation, Reading>> };

    template <std::unsigned_integral Lane, typename Relation, Extension Reading>
    constexpr auto IntegerPick = Binary { &mapLanes<Lane, &lanePick<Lane, Relation, Reading>> };

    template <std::unsigned_integral Lane, typename Arithmetic, Extension Reading>
    constexpr auto IntegerSaturating = Binary { &mapLanes<Lane, &laneSaturating<Lane, Arithmetic, Reading>> };

    /// Every SIMD operator, one row each. Memory accesses, v128.const and i8x16.shuffle take
    /// immediates the operators do not, so each executor runs those itself.
    constexpr auto SimdTable = std::to_array<SimdRow>({
        // Bitwise operators
        { .opcode = Opcode::V128Not, .kernel = Unary { &bitNot } },
        { .opcode = Opcode::V128And, .kernel = Binary { &mapLanes<I64, &bitAnd<I64>> } },
        { .opcode = Opcode::V128Andnot, .kernel = Binary { &bitAndNot } },
        { .opcode = Opcode::V128Or, .kernel = Binary { &mapLanes<I64, &bitOr<I64>> } },
        { .opcode = Opcode::V128Xor, .kernel = Binary { &mapLanes<I64, &bitXor<I64>> } },
        { .opcode = Opcode::V128Bitselect, .kernel = Ternary { &bitSelect } },
        { .opcode = Opcode::V128AnyTrue, .kernel = Test { &anyTrue } },
        { .opcode = Opcode::I8x16Swizzle, .kernel = Binary { &swizzle } },
        // Shifts
        { .opcode = Opcode::I8x16Shl, .kernel = Shift { &shiftLanes<uint8_t, ShiftKind::Left> } },
        { .opcode = Opcode::I8x16ShrS, .kernel = Shift { &shiftLanes<uint8_t, ShiftKind::RightSigned> } },
        { .opcode = Opcode::I8x16ShrU, .kernel = Shift { &shiftLanes<uint8_t, ShiftKind::RightUnsigned> } },
        { .opcode = Opcode::I16x8Shl, .kernel = Shift { &shiftLanes<uint16_t, ShiftKind::Left> } },
        { .opcode = Opcode::I16x8ShrS, .kernel = Shift { &shiftLanes<uint16_t, ShiftKind::RightSigned> } },
        { .opcode = Opcode::I16x8ShrU, .kernel = Shift { &shiftLanes<uint16_t, ShiftKind::RightUnsigned> } },
        { .opcode = Opcode::I32x4Shl, .kernel = Shift { &shiftLanes<uint32_t, ShiftKind::Left> } },
        { .opcode = Opcode::I32x4ShrS, .kernel = Shift { &shiftLanes<uint32_t, ShiftKind::RightSigned> } },
        { .opcode = Opcode::I32x4ShrU, .kernel = Shift { &shiftLanes<uint32_t, ShiftKind::RightUnsigned> } },
        { .opcode = Opcode::I64x2Shl, .kernel = Shift { &shiftLanes<uint64_t, ShiftKind::Left> } },
        { .opcode = Opcode::I64x2ShrS, .kernel = Shift { &shiftLanes<uint64_t, ShiftKind::RightSigned> } },
        { .opcode = Opcode::I64x2ShrU, .kernel = Shift { &shiftLanes<uint64_t, ShiftKind::RightUnsigned> } },
        // Integer arithmetic
        { .opcode = Opcode::I8x16Add, .kernel = Binary { &mapHalves<&packedAdd<uint8_t>> } },
        { .opcode = Opcode::I8x16Sub, .kernel = Binary { &mapHalves<&packedSub<uint8_t>> } },
        { .opcode = Opcode::I8x16Neg, .kernel = Unary { &mapLanes<uint8_t, &laneNeg<uint8_t>> } },
        { .opcode = Opcode::I8x16Abs, .kernel = Unary { &mapLanes<uint8_t, &laneAbs<uint8_t>> } },
        { .opcode = Opcode::I16x8Add, .kernel = Binary { &mapHalves<&packedAdd<uint16_t>> } },
        { .opcode = Opcode::I16x8Sub, .kernel = Binary { &mapHalves<&packedSub<uint16_t>> } },
        { .opcode = Opcode::I16x8Mul, .kernel = Binary { &mapLanes<uint16_t, &laneMul<uint16_t>> } },
        { .opcode = Opcode::I16x8Neg, .kernel = Unary { &mapLanes<uint16_t, &laneNeg<uint16_t>> } },
        { .opcode = Opcode::I16x8Abs, .kernel = Unary { &mapLanes<uint16_t, &laneAbs<uint16_t>> } },
        { .opcode = Opcode::I32x4Add, .kernel = Binary { &mapHalves<&packedAdd<uint32_t>> } },
        { .opcode = Opcode::I32x4Sub, .kernel = Binary { &mapHalves<&packedSub<uint32_t>> } },
        { .opcode = Opcode::I32x4Mul, .kernel = Binary { &mapLanes<uint32_t, &laneMul<uint32_t>> } },
        { .opcode = Opcode::I32x4Neg, .kernel = Unary { &mapLanes<uint32_t, &laneNeg<uint32_t>> } },
        { .opcode = Opcode::I32x4Abs, .kernel = Unary { &mapLanes<uint32_t, &laneAbs<uint32_t>> } },
        { .opcode = Opcode::I64x2Add, .kernel = Binary { &mapHalves<&packedAdd<uint64_t>> } },
        { .opcode = Opcode::I64x2Sub, .kernel = Binary { &mapHalves<&packedSub<uint64_t>> } },
        { .opcode = Opcode::I64x2Mul, .kernel = Binary { &mapLanes<uint64_t, &laneMul<uint64_t>> } },
        { .opcode = Opcode::I64x2Neg, .kernel = Unary { &mapLanes<uint64_t, &laneNeg<uint64_t>> } },
        { .opcode = Opcode::I64x2Abs, .kernel = Unary { &mapLanes<uint64_t, &laneAbs<uint64_t>> } },
        { .opcode = Opcode::I8x16Popcnt, .kernel = Unary { &mapLanes<uint8_t, &lanePopcnt<uint8_t>> } },
        // Minimum, maximum and average
        { .opcode = Opcode::I8x16MinS, .kernel = IntegerPick<uint8_t, std::less<>, Signed> },
        { .opcode = Opcode::I8x16MinU, .kernel = IntegerPick<uint8_t, std::less<>, Unsigned> },
        { .opcode = Opcode::I8x16MaxS, .kernel = IntegerPick<uint8_t, std::greater<>, Signed> },
        { .opcode = Opcode::I8x16MaxU, .kernel = IntegerPick<uint8_t, std::greater<>, Unsigned> },
        { .opcode = Opcode::I16x8MinS, .kernel = IntegerPick<uint16_t, std::less<>, Signed> },
        { .opcode = Opcode::I16x8MinU, .kernel = IntegerPick<uint16_t, std::less<>, Unsigned> },
        { .opcode = Opcode::I16x8MaxS, .kernel = IntegerPick<uint16_t, std::greater<>, Signed> },
        { .opcode = Opcode::I16x8MaxU, .kernel = IntegerPick<uint16_t, std::greater<>, Unsigned> },
        { .opcode = Opcode::I32x4MinS, .kernel = IntegerPick<uint32_t, std::less<>, Signed> },
        { .opcode = Opcode::I32x4MinU, .kernel = IntegerPick<uint32_t, std::less<>, Unsigned> },
        { .opcode = Opcode::I32x4MaxS, .kernel = IntegerPick<uint32_t, std::greater<>, Signed> },
        { .opcode = Opcode::I32x4MaxU, .kernel = IntegerPick<uint32_t, std::greater<>, Unsigned> },
        { .opcode = Opcode::I8x16AvgrU,
          .kernel = Binary { &mapLanes<uint8_t, &laneAverageRounded<uint8_t>> } },
        { .opcode = Opcode::I16x8AvgrU,
          .kernel = Binary { &mapLanes<uint16_t, &laneAverageRounded<uint16_t>> } },
        // Saturating arithmetic
        { .opcode = Opcode::I8x16AddSatS, .kernel = IntegerSaturating<uint8_t, std::plus<>, Signed> },
        { .opcode = Opcode::I8x16AddSatU, .kernel = IntegerSaturating<uint8_t, std::plus<>, Unsigned> },
        { .opcode = Opcode::I8x16SubSatS, .kernel = IntegerSaturating<uint8_t, std::minus<>, Signed> },
        { .opcode = Opcode::I8x16SubSatU, .kernel = IntegerSaturating<uint8_t, std::minus<>, Unsigned> },
        { .opcode = Opcode::I16x8AddSatS, .kernel = IntegerSaturating<uint16_t, std::plus<>, Signed> },
        { .opcode = Opcode::I16x8AddSatU, .kernel = IntegerSaturating<uint16_t, std::plus<>, Unsigned> },
        { .opcode = Opcode::I16x8SubSatS, .kernel = IntegerSaturating<uint16_t, std::minus<>, Signed> },
        { .opcode = Opcode::I16x8SubSatU, .kernel = IntegerSaturating<uint16_t, std::minus<>, Unsigned> },
        { .opcode = Opcode::I16x8Q15mulrSatS,
          .kernel = Binary { &mapLanes<uint16_t, &laneQ15MulRoundSaturate> } },
        // Comparisons
        { .opcode = Opcode::I8x16Eq, .kernel = IntegerCompare<uint8_t, std::equal_to<>, Unsigned> },
        { .opcode = Opcode::I8x16Ne, .kernel = IntegerCompare<uint8_t, std::not_equal_to<>, Unsigned> },
        { .opcode = Opcode::I8x16LtS, .kernel = IntegerCompare<uint8_t, std::less<>, Signed> },
        { .opcode = Opcode::I8x16LtU, .kernel = IntegerCompare<uint8_t, std::less<>, Unsigned> },
        { .opcode = Opcode::I8x16GtS, .kernel = IntegerCompare<uint8_t, std::greater<>, Signed> },
        { .opcode = Opcode::I8x16GtU, .kernel = IntegerCompare<uint8_t, std::greater<>, Unsigned> },
        { .opcode = Opcode::I8x16LeS, .kernel = IntegerCompare<uint8_t, std::less_equal<>, Signed> },
        { .opcode = Opcode::I8x16LeU, .kernel = IntegerCompare<uint8_t, std::less_equal<>, Unsigned> },
        { .opcode = Opcode::I8x16GeS, .kernel = IntegerCompare<uint8_t, std::greater_equal<>, Signed> },
        { .opcode = Opcode::I8x16GeU, .kernel = IntegerCompare<uint8_t, std::greater_equal<>, Unsigned> },
        { .opcode = Opcode::I16x8Eq, .kernel = IntegerCompare<uint16_t, std::equal_to<>, Unsigned> },
        { .opcode = Opcode::I16x8Ne, .kernel = IntegerCompare<uint16_t, std::not_equal_to<>, Unsigned> },
        { .opcode = Opcode::I16x8LtS, .kernel = IntegerCompare<uint16_t, std::less<>, Signed> },
        { .opcode = Opcode::I16x8LtU, .kernel = IntegerCompare<uint16_t, std::less<>, Unsigned> },
        { .opcode = Opcode::I16x8GtS, .kernel = IntegerCompare<uint16_t, std::greater<>, Signed> },
        { .opcode = Opcode::I16x8GtU, .kernel = IntegerCompare<uint16_t, std::greater<>, Unsigned> },
        { .opcode = Opcode::I16x8LeS, .kernel = IntegerCompare<uint16_t, std::less_equal<>, Signed> },
        { .opcode = Opcode::I16x8LeU, .kernel = IntegerCompare<uint16_t, std::less_equal<>, Unsigned> },
        { .opcode = Opcode::I16x8GeS, .kernel = IntegerCompare<uint16_t, std::greater_equal<>, Signed> },
        { .opcode = Opcode::I16x8GeU, .kernel = IntegerCompare<uint16_t, std::greater_equal<>, Unsigned> },
        { .opcode = Opcode::I32x4Eq, .kernel = IntegerCompare<uint32_t, std::equal_to<>, Unsigned> },
        { .opcode = Opcode::I32x4Ne, .kernel = IntegerCompare<uint32_t, std::not_equal_to<>, Unsigned> },
        { .opcode = Opcode::I32x4LtS, .kernel = IntegerCompare<uint32_t, std::less<>, Signed> },
        { .opcode = Opcode::I32x4LtU, .kernel = IntegerCompare<uint32_t, std::less<>, Unsigned> },
        { .opcode = Opcode::I32x4GtS, .kernel = IntegerCompare<uint32_t, std::greater<>, Signed> },
        { .opcode = Opcode::I32x4GtU, .kernel = IntegerCompare<uint32_t, std::greater<>, Unsigned> },
        { .opcode = Opcode::I32x4LeS, .kernel = IntegerCompare<uint32_t, std::less_equal<>, Signed> },
        { .opcode = Opcode::I32x4LeU, .kernel = IntegerCompare<uint32_t, std::less_equal<>, Unsigned> },
        { .opcode = Opcode::I32x4GeS, .kernel = IntegerCompare<uint32_t, std::greater_equal<>, Signed> },
        { .opcode = Opcode::I32x4GeU, .kernel = IntegerCompare<uint32_t, std::greater_equal<>, Unsigned> },
        { .opcode = Opcode::I64x2Eq, .kernel = IntegerCompare<uint64_t, std::equal_to<>, Unsigned> },
        { .opcode = Opcode::I64x2Ne, .kernel = IntegerCompare<uint64_t, std::not_equal_to<>, Unsigned> },
        { .opcode = Opcode::I64x2LtS, .kernel = IntegerCompare<uint64_t, std::less<>, Signed> },
        { .opcode = Opcode::I64x2GtS, .kernel = IntegerCompare<uint64_t, std::greater<>, Signed> },
        { .opcode = Opcode::I64x2LeS, .kernel = IntegerCompare<uint64_t, std::less_equal<>, Signed> },
        { .opcode = Opcode::I64x2GeS, .kernel = IntegerCompare<uint64_t, std::greater_equal<>, Signed> },
        // Lane tests
        { .opcode = Opcode::I8x16AllTrue, .kernel = Test { &allTrue<uint8_t> } },
        { .opcode = Opcode::I8x16Bitmask, .kernel = Test { &bitmask<uint8_t> } },
        { .opcode = Opcode::I16x8AllTrue, .kernel = Test { &allTrue<uint16_t> } },
        { .opcode = Opcode::I16x8Bitmask, .kernel = Test { &bitmask<uint16_t> } },
        { .opcode = Opcode::I32x4AllTrue, .kernel = Test { &allTrue<uint32_t> } },
        { .opcode = Opcode::I32x4Bitmask, .kernel = Test { &bitmask<uint32_t> } },
        { .opcode = Opcode::I64x2AllTrue, .kernel = Test { &allTrue<uint64_t> } },
        { .opcode = Opcode::I64x2Bitmask, .kernel = Test { &bitmask<uint64_t> } },
        // Widening and narrowing
        { .opcode = Opcode::I16x8ExtendLowI8x16S,
          .kernel = Unary { &extendLanes<uint8_t, uint16_t, Half::Low, Signed> } },
        { .opcode = Opcode::I16x8ExtendLowI8x16U,
          .kernel = Unary { &extendLanes<uint8_t, uint16_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I16x8ExtendHighI8x16S,
          .kernel = Unary { &extendLanes<uint8_t, uint16_t, Half::High, Signed> } },
        { .opcode = Opcode::I16x8ExtendHighI8x16U,
          .kernel = Unary { &extendLanes<uint8_t, uint16_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I32x4ExtendLowI16x8S,
          .kernel = Unary { &extendLanes<uint16_t, uint32_t, Half::Low, Signed> } },
        { .opcode = Opcode::I32x4ExtendLowI16x8U,
          .kernel = Unary { &extendLanes<uint16_t, uint32_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I32x4ExtendHighI16x8S,
          .kernel = Unary { &extendLanes<uint16_t, uint32_t, Half::High, Signed> } },
        { .opcode = Opcode::I32x4ExtendHighI16x8U,
          .kernel = Unary { &extendLanes<uint16_t, uint32_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I64x2ExtendLowI32x4S,
          .kernel = Unary { &extendLanes<uint32_t, uint64_t, Half::Low, Signed> } },
        { .opcode = Opcode::I64x2ExtendLowI32x4U,
          .kernel = Unary { &extendLanes<uint32_t, uint64_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I64x2ExtendHighI32x4S,
          .kernel = Unary { &extendLanes<uint32_t, uint64_t, Half::High, Signed> } },
        { .opcode = Opcode::I64x2ExtendHighI32x4U,
          .kernel = Unary { &extendLanes<uint32_t, uint64_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I16x8ExtmulLowI8x16S,
          .kernel = Binary { &extendMultiply<uint8_t, uint16_t, Half::Low, Signed> } },
        { .opcode = Opcode::I16x8ExtmulLowI8x16U,
          .kernel = Binary { &extendMultiply<uint8_t, uint16_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I16x8ExtmulHighI8x16S,
          .kernel = Binary { &extendMultiply<uint8_t, uint16_t, Half::High, Signed> } },
        { .opcode = Opcode::I16x8ExtmulHighI8x16U,
          .kernel = Binary { &extendMultiply<uint8_t, uint16_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I32x4ExtmulLowI16x8S,
          .kernel = Binary { &extendMultiply<uint16_t, uint32_t, Half::Low, Signed> } },
        { .opcode = Opcode::I32x4ExtmulLowI16x8U,
          .kernel = Binary { &extendMultiply<uint16_t, uint32_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I32x4ExtmulHighI16x8S,
          .kernel = Binary { &extendMultiply<uint16_t, uint32_t, Half::High, Signed> } },
        { .opcode = Opcode::I32x4ExtmulHighI16x8U,
          .kernel = Binary { &extendMultiply<uint16_t, uint32_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I64x2ExtmulLowI32x4S,
          .kernel = Binary { &extendMultiply<uint32_t, uint64_t, Half::Low, Signed> } },
        { .opcode = Opcode::I64x2ExtmulLowI32x4U,
          .kernel = Binary { &extendMultiply<uint32_t, uint64_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I64x2ExtmulHighI32x4S,
          .kernel = Binary { &extendMultiply<uint32_t, uint64_t, Half::High, Signed> } },
        { .opcode = Opcode::I64x2ExtmulHighI32x4U,
          .kernel = Binary { &extendMultiply<uint32_t, uint64_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I16x8ExtaddPairwiseI8x16S,
          .kernel = Unary { &extendAddPairwise<uint8_t, uint16_t, Signed> } },
        { .opcode = Opcode::I16x8ExtaddPairwiseI8x16U,
          .kernel = Unary { &extendAddPairwise<uint8_t, uint16_t, Unsigned> } },
        { .opcode = Opcode::I32x4ExtaddPairwiseI16x8S,
          .kernel = Unary { &extendAddPairwise<uint16_t, uint32_t, Signed> } },
        { .opcode = Opcode::I32x4ExtaddPairwiseI16x8U,
          .kernel = Unary { &extendAddPairwise<uint16_t, uint32_t, Unsigned> } },
        { .opcode = Opcode::I32x4DotI16x8S, .kernel = Binary { &mapLanes<uint32_t, &dotLane> } },
        { .opcode = Opcode::I8x16NarrowI16x8S, .kernel = Binary { &narrowLanes<uint16_t, uint8_t, Signed> } },
        { .opcode = Opcode::I8x16NarrowI16x8U,
          .kernel = Binary { &narrowLanes<uint16_t, uint8_t, Unsigned> } },
        { .opcode = Opcode::I16x8NarrowI32x4S,
          .kernel = Binary { &narrowLanes<uint32_t, uint16_t, Signed> } },
        { .opcode = Opcode::I16x8NarrowI32x4U,
          .kernel = Binary { &narrowLanes<uint32_t, uint16_t, Unsigned> } },
        // Float arithmetic
        { .opcode = Opcode::F32x4Abs, .kernel = Unary { &mapLanes<F32, &fabs<F32>> } },
        { .opcode = Opcode::F32x4Neg, .kernel = Unary { &mapLanes<F32, &fneg<F32>> } },
        { .opcode = Opcode::F32x4Sqrt, .kernel = Unary { &mapLanes<F32, &sqrt<F32>> } },
        { .opcode = Opcode::F32x4Add, .kernel = Binary { &mapLanes<F32, &fadd<F32>> } },
        { .opcode = Opcode::F32x4Sub, .kernel = Binary { &mapLanes<F32, &fsub<F32>> } },
        { .opcode = Opcode::F32x4Mul, .kernel = Binary { &mapLanes<F32, &fmul<F32>> } },
        { .opcode = Opcode::F32x4Div, .kernel = Binary { &mapLanes<F32, &fdiv<F32>> } },
        { .opcode = Opcode::F32x4Min, .kernel = Binary { &mapLanes<F32, &fmin<F32>> } },
        { .opcode = Opcode::F32x4Max, .kernel = Binary { &mapLanes<F32, &fmax<F32>> } },
        { .opcode = Opcode::F32x4Pmin, .kernel = Binary { &mapLanes<F32, &pseudoMinimum<F32>> } },
        { .opcode = Opcode::F32x4Pmax, .kernel = Binary { &mapLanes<F32, &pseudoMaximum<F32>> } },
        { .opcode = Opcode::F64x2Abs, .kernel = Unary { &mapLanes<F64, &fabs<F64>> } },
        { .opcode = Opcode::F64x2Neg, .kernel = Unary { &mapLanes<F64, &fneg<F64>> } },
        { .opcode = Opcode::F64x2Sqrt, .kernel = Unary { &mapLanes<F64, &sqrt<F64>> } },
        { .opcode = Opcode::F64x2Add, .kernel = Binary { &mapLanes<F64, &fadd<F64>> } },
        { .opcode = Opcode::F64x2Sub, .kernel = Binary { &mapLanes<F64, &fsub<F64>> } },
        { .opcode = Opcode::F64x2Mul, .kernel = Binary { &mapLanes<F64, &fmul<F64>> } },
        { .opcode = Opcode::F64x2Div, .kernel = Binary { &mapLanes<F64, &fdiv<F64>> } },
        { .opcode = Opcode::F64x2Min, .kernel = Binary { &mapLanes<F64, &fmin<F64>> } },
        { .opcode = Opcode::F64x2Max, .kernel = Binary { &mapLanes<F64, &fmax<F64>> } },
        { .opcode = Opcode::F64x2Pmin, .kernel = Binary { &mapLanes<F64, &pseudoMinimum<F64>> } },
        { .opcode = Opcode::F64x2Pmax, .kernel = Binary { &mapLanes<F64, &pseudoMaximum<F64>> } },
        // Rounding to an integral value
        { .opcode = Opcode::F32x4Ceil, .kernel = Unary { &mapLanes<F32, &ceil<F32>> } },
        { .opcode = Opcode::F32x4Floor, .kernel = Unary { &mapLanes<F32, &floor<F32>> } },
        { .opcode = Opcode::F32x4Trunc, .kernel = Unary { &mapLanes<F32, &trunc<F32>> } },
        { .opcode = Opcode::F32x4Nearest, .kernel = Unary { &mapLanes<F32, &nearest<F32>> } },
        { .opcode = Opcode::F64x2Ceil, .kernel = Unary { &mapLanes<F64, &ceil<F64>> } },
        { .opcode = Opcode::F64x2Floor, .kernel = Unary { &mapLanes<F64, &floor<F64>> } },
        { .opcode = Opcode::F64x2Trunc, .kernel = Unary { &mapLanes<F64, &trunc<F64>> } },
        { .opcode = Opcode::F64x2Nearest, .kernel = Unary { &mapLanes<F64, &nearest<F64>> } },
        // Float comparisons
        { .opcode = Opcode::F32x4Eq, .kernel = Binary { &mapLanes<F32, &feq<F32>> } },
        { .opcode = Opcode::F32x4Ne, .kernel = Binary { &mapLanes<F32, &fne<F32>> } },
        { .opcode = Opcode::F32x4Lt, .kernel = Binary { &mapLanes<F32, &flt<F32>> } },
        { .opcode = Opcode::F32x4Gt, .kernel = Binary { &mapLanes<F32, &fgt<F32>> } },
        { .opcode = Opcode::F32x4Le, .kernel = Binary { &mapLanes<F32, &fle<F32>> } },
        { .opcode = Opcode::F32x4Ge, .kernel = Binary { &mapLanes<F32, &fge<F32>> } },
        { .opcode = Opcode::F64x2Eq, .kernel = Binary { &mapLanes<F64, &feq<F64>> } },
        { .opcode = Opcode::F64x2Ne, .kernel = Binary { &mapLanes<F64, &fne<F64>> } },
        { .opcode = Opcode::F64x2Lt, .kernel = Binary { &mapLanes<F64, &flt<F64>> } },
        { .opcode = Opcode::F64x2Gt, .kernel = Binary { &mapLanes<F64, &fgt<F64>> } },
        { .opcode = Opcode::F64x2Le, .kernel = Binary { &mapLanes<F64, &fle<F64>> } },
        { .opcode = Opcode::F64x2Ge, .kernel = Binary { &mapLanes<F64, &fge<F64>> } },
        // Conversions
        { .opcode = Opcode::I32x4TruncSatF32x4S,
          .kernel = Unary { &mapLanes<F32, &truncSaturating<I32, int32_t, F32>> } },
        { .opcode = Opcode::I32x4TruncSatF32x4U,
          .kernel = Unary { &mapLanes<F32, &truncSaturating<I32, uint32_t, F32>> } },
        { .opcode = Opcode::I32x4TruncSatF64x2SZero,
          .kernel = Unary { &convertLowLanes<F64, I32, &truncSaturating<I32, int32_t, F64>> } },
        { .opcode = Opcode::I32x4TruncSatF64x2UZero,
          .kernel = Unary { &convertLowLanes<F64, I32, &truncSaturating<I32, uint32_t, F64>> } },
        { .opcode = Opcode::F32x4ConvertI32x4S,
          .kernel = Unary { &mapLanes<I32, &convert<F32, int32_t, I32>> } },
        { .opcode = Opcode::F32x4ConvertI32x4U,
          .kernel = Unary { &mapLanes<I32, &convert<F32, uint32_t, I32>> } },
        { .opcode = Opcode::F64x2ConvertLowI32x4S,
          .kernel = Unary { &convertLowLanes<I32, F64, &convert<F64, int32_t, I32>> } },
        { .opcode = Opcode::F64x2ConvertLowI32x4U,
          .kernel = Unary { &convertLowLanes<I32, F64, &convert<F64, uint32_t, I32>> } },
        { .opcode = Opcode::F32x4DemoteF64x2Zero, .kernel = Unary { &convertLowLanes<F64, F32, &demote> } },
        { .opcode = Opcode::F64x2PromoteLowF32x4, .kernel = Unary { &convertLowLanes<F32, F64, &promote> } },
        // Relaxed SIMD, the Wasmtime-deterministic profile (SimdKernels.hpp)
        { .opcode = Opcode::I8x16RelaxedSwizzle, .kernel = Binary { &swizzle } },
        { .opcode = Opcode::I32x4RelaxedTruncF32x4S,
          .kernel = Unary { &mapLanes<F32, &truncSaturating<I32, int32_t, F32>> } },
        { .opcode = Opcode::I32x4RelaxedTruncF32x4U,
          .kernel = Unary { &mapLanes<F32, &truncSaturating<I32, uint32_t, F32>> } },
        { .opcode = Opcode::I32x4RelaxedTruncF64x2SZero,
          .kernel = Unary { &convertLowLanes<F64, I32, &truncSaturating<I32, int32_t, F64>> } },
        { .opcode = Opcode::I32x4RelaxedTruncF64x2UZero,
          .kernel = Unary { &convertLowLanes<F64, I32, &truncSaturating<I32, uint32_t, F64>> } },
        { .opcode = Opcode::F32x4RelaxedMadd,
          .kernel = Ternary { &mapLanes<F32, &fusedMultiplyAdd<F32, ProductSign::Positive>> } },
        { .opcode = Opcode::F32x4RelaxedNmadd,
          .kernel = Ternary { &mapLanes<F32, &fusedMultiplyAdd<F32, ProductSign::Negated>> } },
        { .opcode = Opcode::F64x2RelaxedMadd,
          .kernel = Ternary { &mapLanes<F64, &fusedMultiplyAdd<F64, ProductSign::Positive>> } },
        { .opcode = Opcode::F64x2RelaxedNmadd,
          .kernel = Ternary { &mapLanes<F64, &fusedMultiplyAdd<F64, ProductSign::Negated>> } },
        { .opcode = Opcode::I8x16RelaxedLaneselect, .kernel = Ternary { &bitSelect } },
        { .opcode = Opcode::I16x8RelaxedLaneselect, .kernel = Ternary { &bitSelect } },
        { .opcode = Opcode::I32x4RelaxedLaneselect, .kernel = Ternary { &bitSelect } },
        { .opcode = Opcode::I64x2RelaxedLaneselect, .kernel = Ternary { &bitSelect } },
        { .opcode = Opcode::F32x4RelaxedMin, .kernel = Binary { &mapLanes<F32, &fmin<F32>> } },
        { .opcode = Opcode::F32x4RelaxedMax, .kernel = Binary { &mapLanes<F32, &fmax<F32>> } },
        { .opcode = Opcode::F64x2RelaxedMin, .kernel = Binary { &mapLanes<F64, &fmin<F64>> } },
        { .opcode = Opcode::F64x2RelaxedMax, .kernel = Binary { &mapLanes<F64, &fmax<F64>> } },
        { .opcode = Opcode::I16x8RelaxedQ15mulrS,
          .kernel = Binary { &mapLanes<uint16_t, &laneQ15MulRoundSaturate> } },
        { .opcode = Opcode::I16x8RelaxedDotI8x16I7x16S,
          .kernel = Binary { &mapLanes<uint16_t, &relaxedDotLane> } },
        { .opcode = Opcode::I32x4RelaxedDotI8x16I7x16AddS,
          .kernel = Ternary { &mapLanes<uint32_t, &relaxedDotAddLane> } },
        // Lane operations: each executor's dedicated rows
        { .opcode = Opcode::I8x16Splat, .kernel = Splat {} },
        { .opcode = Opcode::I16x8Splat, .kernel = Splat {} },
        { .opcode = Opcode::I32x4Splat, .kernel = Splat {} },
        { .opcode = Opcode::I64x2Splat, .kernel = Splat {} },
        { .opcode = Opcode::F32x4Splat, .kernel = Splat {} },
        { .opcode = Opcode::F64x2Splat, .kernel = Splat {} },
        { .opcode = Opcode::I8x16ExtractLaneS, .kernel = Extract {} },
        { .opcode = Opcode::I8x16ExtractLaneU, .kernel = Extract {} },
        { .opcode = Opcode::I16x8ExtractLaneS, .kernel = Extract {} },
        { .opcode = Opcode::I16x8ExtractLaneU, .kernel = Extract {} },
        { .opcode = Opcode::I32x4ExtractLane, .kernel = Extract {} },
        { .opcode = Opcode::I64x2ExtractLane, .kernel = Extract {} },
        { .opcode = Opcode::F32x4ExtractLane, .kernel = Extract {} },
        { .opcode = Opcode::F64x2ExtractLane, .kernel = Extract {} },
        { .opcode = Opcode::I8x16ReplaceLane, .kernel = Replace {} },
        { .opcode = Opcode::I16x8ReplaceLane, .kernel = Replace {} },
        { .opcode = Opcode::I32x4ReplaceLane, .kernel = Replace {} },
        { .opcode = Opcode::I64x2ReplaceLane, .kernel = Replace {} },
        { .opcode = Opcode::F32x4ReplaceLane, .kernel = Replace {} },
        { .opcode = Opcode::F64x2ReplaceLane, .kernel = Replace {} },
    });

    /// Each opcode's kernel, by opcode; monostate for an opcode without a row.
    constexpr auto KernelByOpcode = [] {
        auto table = std::array<Kernel, binary::OpcodeCount> {};
        for (auto const& row: SimdTable)
            table.at(static_cast<std::size_t>(row.opcode)) = row.kernel;
        return table;
    }();

    /// The kernels of type @p Function, by opcode; nullptr where an opcode's row holds another
    /// shape, or it has none.
    template <typename Function>
    [[nodiscard]] consteval std::array<Function, binary::OpcodeCount> kernelsOf() noexcept
    {
        auto kernels = std::array<Function, binary::OpcodeCount> {};
        for (auto const& row: SimdTable)
            if (auto const* const kernel = std::get_if<Function>(&row.kernel))
                kernels.at(static_cast<std::size_t>(row.opcode)) = *kernel;
        return kernels;
    }
} // namespace

namespace detail
{
    constinit std::array<SimdUnaryKernel, binary::OpcodeCount> const unaryKernels =
        kernelsOf<SimdUnaryKernel>();
    constinit std::array<SimdBinaryKernel, binary::OpcodeCount> const binaryKernels =
        kernelsOf<SimdBinaryKernel>();
    constinit std::array<SimdTernaryKernel, binary::OpcodeCount> const ternaryKernels =
        kernelsOf<SimdTernaryKernel>();
    constinit std::array<SimdTestKernel, binary::OpcodeCount> const testKernels = kernelsOf<SimdTestKernel>();
    constinit std::array<SimdShiftKernel, binary::OpcodeCount> const shiftKernels =
        kernelsOf<SimdShiftKernel>();
} // namespace detail

SimdShape simdShape(Opcode opcode) noexcept
{
    return static_cast<SimdShape>(KernelByOpcode[static_cast<std::size_t>(opcode)].index());
}

} // namespace isoline::numerics
