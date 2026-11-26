// SPDX-License-Identifier: Apache-2.0
#include <isoline/interpreter/Simd.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <type_traits>
#include <variant>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/interpreter/Numerics.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::interpreter
{

namespace
{
    using binary::Extension;
    using binary::Opcode;
    using runtime::F32;
    using runtime::F64;
    using runtime::I32;
    using runtime::I64;
    using runtime::V128;
    using runtime::Value;

    // --- Lane-wise helpers --------------------------------------------------------------------

    /// The vector whose every lane of type @p Lane is @p fn of that lane of @p operand.
    template <std::unsigned_integral Lane, typename Fn>
    [[nodiscard]] constexpr V128 lanewise(V128 const& operand, Fn fn) noexcept
    {
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, LaneCount<Lane>))
            setLane<Lane>(result, index, fn(laneOf<Lane>(operand, index)));
        return result;
    }

    /// The vector whose every lane of type @p Lane is @p fn of that lane of @p lhs and of @p rhs.
    template <std::unsigned_integral Lane, typename Fn>
    [[nodiscard]] constexpr V128 lanewise(V128 const& lhs, V128 const& rhs, Fn fn) noexcept
    {
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, LaneCount<Lane>))
            setLane<Lane>(result, index, fn(laneOf<Lane>(lhs, index), laneOf<Lane>(rhs, index)));
        return result;
    }

    /// The bit width of a lane of type @p Lane.
    template <std::unsigned_integral Lane>
    inline constexpr auto LaneBits = unsigned { std::numeric_limits<Lane>::digits };

    /// The bits @p value holds, of the scalar type @p Box. Precondition (checked): it holds one.
    template <typename Box>
    [[nodiscard]] auto bitsOf(Value const& value)
    {
        Require(std::holds_alternative<Box>(value));
        return std::get<Box>(value).bits;
    }

    // --- Bitwise operators --------------------------------------------------------------------

    [[nodiscard]] constexpr V128 bitNot(V128 operand) noexcept
    {
        return V128 { .lanes = { ~operand.lanes[0], ~operand.lanes[1] } };
    }

    [[nodiscard]] constexpr V128 bitAnd(V128 lhs, V128 rhs) noexcept
    {
        return V128 { .lanes = { lhs.lanes[0] & rhs.lanes[0], lhs.lanes[1] & rhs.lanes[1] } };
    }

    [[nodiscard]] constexpr V128 bitAndNot(V128 lhs, V128 rhs) noexcept
    {
        return bitAnd(lhs, bitNot(rhs));
    }

    [[nodiscard]] constexpr V128 bitOr(V128 lhs, V128 rhs) noexcept
    {
        return V128 { .lanes = { lhs.lanes[0] | rhs.lanes[0], lhs.lanes[1] | rhs.lanes[1] } };
    }

    [[nodiscard]] constexpr V128 bitXor(V128 lhs, V128 rhs) noexcept
    {
        return V128 { .lanes = { lhs.lanes[0] ^ rhs.lanes[0], lhs.lanes[1] ^ rhs.lanes[1] } };
    }

    /// v128.bitselect: each bit from @p first where @p mask's is set, else from @p second.
    [[nodiscard]] constexpr V128 bitSelect(V128 first, V128 second, V128 mask) noexcept
    {
        return bitOr(bitAnd(first, mask), bitAndNot(second, mask));
    }

    [[nodiscard]] constexpr I32 anyTrue(V128 operand) noexcept
    {
        return I32 { (operand.lanes[0] | operand.lanes[1]) != 0 ? 1U : 0U };
    }

    /// i8x16.swizzle: byte i is the byte of @p bytes that byte i of @p indices names, or 0 when
    /// that index is 16 or more.
    [[nodiscard]] constexpr V128 swizzle(V128 bytes, V128 indices) noexcept
    {
        return lanewise<uint8_t>(indices, [&](uint8_t index) {
            return index < LaneCount<uint8_t> ? laneOf<uint8_t>(bytes, index) : uint8_t { 0 };
        });
    }

    // --- Shifts -------------------------------------------------------------------------------

    /// Which way a lane-wise shift moves its bits.
    enum class ShiftKind : uint8_t
    {
        Left,          ///< shl.
        RightSigned,   ///< shr_s: the sign bit fills in.
        RightUnsigned, ///< shr_u: zeros fill in.
    };

    template <std::unsigned_integral Lane, ShiftKind Kind>
    [[nodiscard]] constexpr V128 shiftLanes(V128 operand, uint32_t count) noexcept
    {
        auto const distance = count % LaneBits<Lane>;
        return lanewise<Lane>(operand, [distance](Lane lane) {
            if constexpr (Kind == ShiftKind::Left)
                return static_cast<Lane>(lane << distance);
            else if constexpr (Kind == ShiftKind::RightSigned)
                return static_cast<Lane>(static_cast<std::make_signed_t<Lane>>(lane) >> distance);
            else
                return static_cast<Lane>(lane >> distance);
        });
    }

    // --- Integer arithmetic -------------------------------------------------------------------

    /// An integer operator reads its lanes as signed (the `_s` forms) ...
    constexpr auto Signed = Extension::Sign;
    /// ... or as unsigned (the `_u` forms, and those for which the sign does not matter).
    constexpr auto Unsigned = Extension::Zero;

    /// The type @p Lane's arithmetic is done in: at least unsigned int, so a narrow lane's sum or
    /// product wraps instead of overflowing the int it would promote to.
    template <std::unsigned_integral Lane>
    using Promoted = decltype(Lane {} + 0U);

    /// @p lane read as signed when @p Interpretation is Sign, else as unsigned.
    template <Extension Interpretation, std::unsigned_integral Lane>
    [[nodiscard]] constexpr auto interpreted(Lane lane) noexcept
    {
        if constexpr (Interpretation == Extension::Sign)
            return static_cast<std::make_signed_t<Lane>>(lane);
        else
            return lane;
    }

    /// @p lane widened to @p Wide, sign- or zero-extended per @p Widening.
    template <std::unsigned_integral Wide, Extension Widening, std::unsigned_integral Lane>
    [[nodiscard]] constexpr Wide widened(Lane lane) noexcept
    {
        return static_cast<Wide>(static_cast<std::make_signed_t<Wide>>(interpreted<Widening>(lane)));
    }

    /// @p value clamped to the range of @p Lane read per @p Interpretation, as the lane's bits.
    template <std::unsigned_integral Lane, Extension Interpretation>
    [[nodiscard]] constexpr Lane saturated(int64_t value) noexcept
    {
        using Target = decltype(interpreted<Interpretation>(Lane {}));
        auto const low = int64_t { std::numeric_limits<Target>::min() };
        auto const high = int64_t { std::numeric_limits<Target>::max() };
        return static_cast<Lane>(std::clamp(value, low, high));
    }

    /// A lane-wise binary operator, @p Op::apply of each pair of lanes of type @p Lane.
    template <std::unsigned_integral Lane, typename Op>
    [[nodiscard]] constexpr V128 binaryLanes(V128 lhs, V128 rhs) noexcept
    {
        return lanewise<Lane>(lhs, rhs, [](Lane a, Lane b) { return Op::template apply<Lane>(a, b); });
    }

    /// A lane-wise unary operator, @p Op::apply of each lane of type @p Lane.
    template <std::unsigned_integral Lane, typename Op>
    [[nodiscard]] constexpr V128 unaryLanes(V128 operand) noexcept
    {
        return lanewise<Lane>(operand, [](Lane a) { return Op::template apply<Lane>(a); });
    }

    /// Wrapping addition.
    struct Add
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a, Lane b) noexcept
        {
            return static_cast<Lane>(Promoted<Lane> { a } + b);
        }
    };

    /// Wrapping subtraction.
    struct Sub
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a, Lane b) noexcept
        {
            return static_cast<Lane>(Promoted<Lane> { a } - b);
        }
    };

    /// Wrapping multiplication: the low half of the product.
    struct Mul
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a, Lane b) noexcept
        {
            return static_cast<Lane>(Promoted<Lane> { a } * b);
        }
    };

    /// Wrapping negation.
    struct Neg
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a) noexcept
        {
            return static_cast<Lane>(Promoted<Lane> { 0 } - a);
        }
    };

    /// The absolute value, read as signed; the most negative lane is its own absolute value.
    struct Abs
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a) noexcept
        {
            return interpreted<Extension::Sign>(a) < 0 ? Neg::apply(a) : a;
        }
    };

    /// The number of set bits.
    struct Popcnt
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a) noexcept
        {
            return static_cast<Lane>(std::popcount(a));
        }
    };

    /// The lane @p Relation prefers, comparing the lanes read per @p Interpretation: std::less
    /// picks the minimum, std::greater the maximum.
    template <typename Relation, Extension Interpretation>
    struct Pick
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a, Lane b) noexcept
        {
            return Relation {}(interpreted<Interpretation>(a), interpreted<Interpretation>(b)) ? a : b;
        }
    };

    /// avgr_u: the unsigned average, rounded up.
    struct AverageRounded
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a, Lane b) noexcept
        {
            return static_cast<Lane>((Promoted<Lane> { a } + b + 1) / 2);
        }
    };

    /// A saturating sum (@p Arithmetic std::plus) or difference (std::minus), the lanes read per
    /// @p Interpretation. Only i8 and i16 lanes saturate, so the exact result fits an int64_t.
    template <typename Arithmetic, Extension Interpretation>
    struct Saturating
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a, Lane b) noexcept
        {
            static_assert(sizeof(Lane) <= 2);
            auto const exact = Arithmetic {}(int64_t { interpreted<Interpretation>(a) },
                                             int64_t { interpreted<Interpretation>(b) });
            return saturated<Lane, Interpretation>(exact);
        }
    };

    /// A comparison: all ones where @p Relation holds of the lanes read per @p Interpretation,
    /// else zero.
    template <typename Relation, Extension Interpretation>
    struct Compare
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a, Lane b) noexcept
        {
            return Relation {}(interpreted<Interpretation>(a), interpreted<Interpretation>(b))
                       ? std::numeric_limits<Lane>::max()
                       : Lane { 0 };
        }
    };

    /// i16x8.q15mulr_sat_s: the Q15 product, rounded half up and saturated.
    struct Q15MulRoundSaturate
    {
        template <std::unsigned_integral Lane>
        static constexpr Lane apply(Lane a, Lane b) noexcept
        {
            static_assert(sizeof(Lane) == 2);
            auto const product =
                int64_t { interpreted<Extension::Sign>(a) } * interpreted<Extension::Sign>(b);
            return saturated<Lane, Extension::Sign>((product + 0x4000) >> 15U);
        }
    };

    // --- Lane tests ---------------------------------------------------------------------------

    /// 1 when every lane is non-zero, else 0.
    template <std::unsigned_integral Lane>
    [[nodiscard]] constexpr I32 allTrue(V128 operand) noexcept
    {
        auto const lanes = std::views::iota(0U, LaneCount<Lane>);
        auto const isSet = [&](unsigned index) {
            return laneOf<Lane>(operand, index) != 0;
        };
        return I32 { std::ranges::all_of(lanes, isSet) ? 1U : 0U };
    }

    /// Bit i of the result is the top bit of lane i.
    template <std::unsigned_integral Lane>
    [[nodiscard]] constexpr I32 bitmask(V128 operand) noexcept
    {
        auto mask = uint32_t { 0 };
        for (auto const index: std::views::iota(0U, LaneCount<Lane>))
            mask |= static_cast<uint32_t>(laneOf<Lane>(operand, index) >> (LaneBits<Lane> - 1)) << index;
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
            auto const lane = laneOf<Narrow>(operand, HalfStart<Narrow, Which> + index);
            setLane<Wide>(result, index, widened<Wide, Widening>(lane));
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
        return binaryLanes<Wide, Mul>(wideLhs, wideRhs);
    }

    /// extadd_pairwise: lane i is the sum of narrow lanes 2i and 2i + 1, each widened to @p Wide.
    template <std::unsigned_integral Narrow, std::unsigned_integral Wide, Extension Widening>
    [[nodiscard]] constexpr V128 extendAddPairwise(V128 operand) noexcept
    {
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, LaneCount<Wide>))
        {
            auto const first = widened<Wide, Widening>(laneOf<Narrow>(operand, 2 * index));
            auto const second = widened<Wide, Widening>(laneOf<Narrow>(operand, (2 * index) + 1));
            setLane<Wide>(result, index, Add::apply(first, second));
        }
        return result;
    }

    /// i32x4.dot_i16x8_s: lane i is the wrapping sum of the signed products of lanes 2i and 2i + 1.
    [[nodiscard]] constexpr V128 dot(V128 lhs, V128 rhs) noexcept
    {
        auto const product = [&](unsigned lane) {
            return widened<uint32_t, Extension::Sign>(laneOf<uint16_t>(lhs, lane))
                   * widened<uint32_t, Extension::Sign>(laneOf<uint16_t>(rhs, lane));
        };
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, LaneCount<uint32_t>))
            setLane<uint32_t>(result, index, product(2 * index) + product((2 * index) + 1));
        return result;
    }

    /// narrow: the @p Wide lanes of @p lhs, then of @p rhs, each read as signed and saturated to a
    /// @p Narrow lane read per @p Interpretation.
    template <std::unsigned_integral Wide, std::unsigned_integral Narrow, Extension Interpretation>
    [[nodiscard]] constexpr V128 narrowLanes(V128 lhs, V128 rhs) noexcept
    {
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, LaneCount<Narrow>))
        {
            auto const& source = index < LaneCount<Wide> ? lhs : rhs;
            auto const lane = interpreted<Extension::Sign>(laneOf<Wide>(source, index % LaneCount<Wide>));
            setLane<Narrow>(result, index, saturated<Narrow, Interpretation>(lane));
        }
        return result;
    }

    // --- Floating-point operators and conversions -----------------------------------------------
    //
    // Each lane goes through the scalar operator of Numerics, on its bits: that is where NaN
    // canonicalisation, rounding on bits and the exact conversions live, so a vector lane computes
    // what its scalar instruction does by construction.

    /// The lane type that holds the bits of the scalar value type @p Box.
    template <typename Box>
    using LaneOf = decltype(Box {}.bits);

    /// The result of a scalar operator that cannot trap: every float operator and conversion SIMD
    /// lifts, trunc_sat included. Precondition (checked): it did not trap.
    [[nodiscard]] Value untrapped(std::expected<Value, runtime::Trap> const& result)
    {
        Require(result.has_value());
        return *result;
    }

    /// The scalar unary operator @p Scalar on each lane of @p operand read as a @p From, giving a
    /// lane of @p To. Where the two lane widths differ, the lower lanes are converted, as many as the
    /// wider shape has, and the rest of the result is zero: promote_low and convert_low read lanes 0
    /// and 1, demote_zero and trunc_sat_zero write them.
    template <Opcode Scalar, typename From, typename To = From>
    [[nodiscard]] V128 unaryScalarLanes(V128 operand) noexcept
    {
        constexpr auto Count = std::min(LaneCount<LaneOf<From>>, LaneCount<LaneOf<To>>);
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, Count))
        {
            auto const lane = From { laneOf<LaneOf<From>>(operand, index) };
            setLane<LaneOf<To>>(result, index, bitsOf<To>(untrapped(applyUnary(Scalar, lane))));
        }
        return result;
    }

    /// The scalar binary operator @p Scalar on each pair of lanes, read as @p Box.
    template <Opcode Scalar, typename Box>
    [[nodiscard]] V128 binaryScalarLanes(V128 lhs, V128 rhs) noexcept
    {
        return lanewise<LaneOf<Box>>(lhs, rhs, [](LaneOf<Box> a, LaneOf<Box> b) {
            return bitsOf<Box>(untrapped(applyBinary(Scalar, Box { a }, Box { b })));
        });
    }

    /// The scalar comparison @p Scalar on each pair of lanes, read as @p Box: all ones where it
    /// holds, else zero.
    template <Opcode Scalar, typename Box>
    [[nodiscard]] V128 compareScalarLanes(V128 lhs, V128 rhs) noexcept
    {
        return lanewise<LaneOf<Box>>(lhs, rhs, [](LaneOf<Box> a, LaneOf<Box> b) {
            return applyCompare(Scalar, Box { a }, Box { b }).bits != 0
                       ? std::numeric_limits<LaneOf<Box>>::max()
                       : LaneOf<Box> { 0 };
        });
    }

    /// Which operand a pseudo-minimum or pseudo-maximum prefers when its comparison fails.
    enum class Pseudo : uint8_t
    {
        Minimum, ///< pmin: rhs when rhs < lhs, else lhs.
        Maximum, ///< pmax: rhs when lhs < rhs, else lhs.
    };

    /// pmin and pmax, the spec's select-based definitions on @p Less, the scalar lt of @p Box: the
    /// result is an operand's bits, a NaN's included, never a canonical NaN.
    template <Pseudo Which, Opcode Less, typename Box>
    [[nodiscard]] V128 pseudoLanes(V128 lhs, V128 rhs) noexcept
    {
        return lanewise<LaneOf<Box>>(lhs, rhs, [](LaneOf<Box> a, LaneOf<Box> b) {
            auto const less = [](LaneOf<Box> x, LaneOf<Box> y) {
                return applyCompare(Less, Box { x }, Box { y }).bits != 0;
            };
            if constexpr (Which == Pseudo::Minimum)
                return less(b, a) ? b : a;
            else
                return less(a, b) ? b : a;
        });
    }

    // --- Relaxed SIMD: the Wasmtime-deterministic profile (Simd.hpp) ----------------------------
    //
    // The relaxed operators that are an existing operator in that profile (swizzle, trunc_sat,
    // bitselect, min, max, q15mulr_sat_s) have that operator's evaluator in the table; only these
    // three have a definition of their own.

    /// Whether relaxed_madd or relaxed_nmadd adds the product or its negation.
    enum class ProductSign : uint8_t
    {
        Positive, ///< relaxed_madd: a * b + c.
        Negated,  ///< relaxed_nmadd: -(a * b) + c.
    };

    /// The host floating-point type of the float value type @p Box, and its canonical NaN.
    template <typename Box>
    struct FloatLane;

    template <>
    struct FloatLane<F32>
    {
        using Float = float;
        static constexpr auto CanonicalNan = CanonicalNanF32;
    };

    template <>
    struct FloatLane<F64>
    {
        using Float = double;
        static constexpr auto CanonicalNan = CanonicalNanF64;
    };

    /// relaxed_madd and relaxed_nmadd on each lane, read as @p Box: the product and the sum rounded
    /// once, as std::fma rounds them exactly on every host (in the default floating-point
    /// environment, which every guest entry establishes, as for add and mul). A NaN result is the
    /// canonical NaN.
    template <typename Box, ProductSign Sign>
    [[nodiscard]] V128 fusedMultiplyAdd(V128 a, V128 b, V128 c) noexcept
    {
        using Bits = LaneOf<Box>;
        using Float = FloatLane<Box>::Float;
        static_assert(sizeof(Bits) == sizeof(Float));
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, LaneCount<Bits>))
        {
            auto const factor = std::bit_cast<Float>(laneOf<Bits>(a, index));
            auto const fused = std::fma(Sign == ProductSign::Negated ? -factor : factor,
                                        std::bit_cast<Float>(laneOf<Bits>(b, index)),
                                        std::bit_cast<Float>(laneOf<Bits>(c, index)));
            setLane<Bits>(
                result, index, std::isnan(fused) ? FloatLane<Box>::CanonicalNan : std::bit_cast<Bits>(fused));
        }
        return result;
    }

    /// The exact sum of the signed products of byte lanes Group * lane to Group * lane + Group - 1
    /// of @p lhs and @p rhs; at most 4 * 128 * 128, so an int32_t holds it.
    template <unsigned Group>
    [[nodiscard]] constexpr int32_t signedByteDot(V128 const& lhs, V128 const& rhs, unsigned lane) noexcept
    {
        auto sum = int32_t { 0 };
        for (auto const index: std::views::iota(Group * lane, Group * (lane + 1)))
            sum += int32_t { interpreted<Extension::Sign>(laneOf<uint8_t>(lhs, index)) }
                   * interpreted<Extension::Sign>(laneOf<uint8_t>(rhs, index));
        return sum;
    }

    /// i16x8.relaxed_dot_i8x16_i7x16_s: lane i is the sum of the signed products of bytes 2i and
    /// 2i + 1, wrapping at 16 bits (two products of -128 sum to 0x8000).
    [[nodiscard]] constexpr V128 relaxedDot(V128 lhs, V128 rhs) noexcept
    {
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, LaneCount<uint16_t>))
            setLane<uint16_t>(result, index, static_cast<uint16_t>(signedByteDot<2>(lhs, rhs, index)));
        return result;
    }

    /// i32x4.relaxed_dot_i8x16_i7x16_add_s: lane i is the exact sum of the signed products of bytes
    /// 4i to 4i + 3, with no 16-bit intermediate, plus lane i of @p addend, wrapping at 32 bits.
    [[nodiscard]] constexpr V128 relaxedDotAdd(V128 lhs, V128 rhs, V128 addend) noexcept
    {
        auto result = V128 {};
        for (auto const index: std::views::iota(0U, LaneCount<uint32_t>))
        {
            auto const dot = static_cast<uint32_t>(signedByteDot<4>(lhs, rhs, index));
            setLane<uint32_t>(result, index, Add::apply(dot, laneOf<uint32_t>(addend, index)));
        }
        return result;
    }

    // --- Splats and lanes ---------------------------------------------------------------------

    template <std::unsigned_integral Lane, typename Box>
    [[nodiscard]] V128 splatOf(Value const& value)
    {
        auto const lane = static_cast<Lane>(bitsOf<Box>(value));
        return lanewise<Lane>(V128 {}, [lane](Lane) { return lane; });
    }

    /// Lane @p index of @p vector as a @p Box; a lane narrower than it is widened per @p Widening.
    template <std::unsigned_integral Lane, typename Box, Extension Widening = Extension::Zero>
    [[nodiscard]] Value extractOf(V128 vector, uint8_t index)
    {
        using Bits = decltype(Box {}.bits);
        return Box { widened<Bits, Widening>(laneOf<Lane>(vector, index)) };
    }

    template <std::unsigned_integral Lane, typename Box>
    [[nodiscard]] V128 replaceOf(V128 vector, uint8_t index, Value const& value)
    {
        setLane<Lane>(vector, index, static_cast<Lane>(bitsOf<Box>(value)));
        return vector;
    }

    // --- The table ----------------------------------------------------------------------------

    using UnaryFn = V128 (*)(V128) noexcept;
    using BinaryFn = V128 (*)(V128, V128) noexcept;
    using TernaryFn = V128 (*)(V128, V128, V128) noexcept;
    using TestFn = I32 (*)(V128) noexcept;
    using ShiftFn = V128 (*)(V128, uint32_t) noexcept;
    using SplatFn = V128 (*)(Value const&);
    using ExtractFn = Value (*)(V128, uint8_t);
    using ReplaceFn = V128 (*)(V128, uint8_t, Value const&);

    /// A SIMD operator's evaluator. Its alternatives are in SimdShape's order, so the alternative a
    /// row holds is its shape: the one fact is stated once.
    using Evaluator = std::
        variant<std::monostate, UnaryFn, BinaryFn, TernaryFn, TestFn, ShiftFn, SplatFn, ExtractFn, ReplaceFn>;

    static_assert(std::variant_size_v<Evaluator> == static_cast<std::size_t>(SimdShape::ReplaceLane) + 1,
                  "Evaluator holds one alternative per SimdShape, in its order");

    /// One SIMD operator: the instruction and its evaluator.
    struct SimdRow
    {
        Opcode opcode;       ///< The instruction.
        Evaluator evaluator; ///< What evaluates it; its alternative is the instruction's shape.
    };

    /// Every SIMD operator, one row each. Memory accesses, v128.const and i8x16.shuffle take
    /// immediates the operators do not, so the interpreter executes those itself.
    constexpr auto SimdTable = std::to_array<SimdRow>({
        // Bitwise operators
        { .opcode = Opcode::V128Not, .evaluator = UnaryFn { &bitNot } },
        { .opcode = Opcode::V128And, .evaluator = BinaryFn { &bitAnd } },
        { .opcode = Opcode::V128Andnot, .evaluator = BinaryFn { &bitAndNot } },
        { .opcode = Opcode::V128Or, .evaluator = BinaryFn { &bitOr } },
        { .opcode = Opcode::V128Xor, .evaluator = BinaryFn { &bitXor } },
        { .opcode = Opcode::V128Bitselect, .evaluator = TernaryFn { &bitSelect } },
        { .opcode = Opcode::V128AnyTrue, .evaluator = TestFn { &anyTrue } },
        { .opcode = Opcode::I8x16Swizzle, .evaluator = BinaryFn { &swizzle } },
        // Shifts
        { .opcode = Opcode::I8x16Shl, .evaluator = ShiftFn { &shiftLanes<uint8_t, ShiftKind::Left> } },
        { .opcode = Opcode::I8x16ShrS,
          .evaluator = ShiftFn { &shiftLanes<uint8_t, ShiftKind::RightSigned> } },
        { .opcode = Opcode::I8x16ShrU,
          .evaluator = ShiftFn { &shiftLanes<uint8_t, ShiftKind::RightUnsigned> } },
        { .opcode = Opcode::I16x8Shl, .evaluator = ShiftFn { &shiftLanes<uint16_t, ShiftKind::Left> } },
        { .opcode = Opcode::I16x8ShrS,
          .evaluator = ShiftFn { &shiftLanes<uint16_t, ShiftKind::RightSigned> } },
        { .opcode = Opcode::I16x8ShrU,
          .evaluator = ShiftFn { &shiftLanes<uint16_t, ShiftKind::RightUnsigned> } },
        { .opcode = Opcode::I32x4Shl, .evaluator = ShiftFn { &shiftLanes<uint32_t, ShiftKind::Left> } },
        { .opcode = Opcode::I32x4ShrS,
          .evaluator = ShiftFn { &shiftLanes<uint32_t, ShiftKind::RightSigned> } },
        { .opcode = Opcode::I32x4ShrU,
          .evaluator = ShiftFn { &shiftLanes<uint32_t, ShiftKind::RightUnsigned> } },
        { .opcode = Opcode::I64x2Shl, .evaluator = ShiftFn { &shiftLanes<uint64_t, ShiftKind::Left> } },
        { .opcode = Opcode::I64x2ShrS,
          .evaluator = ShiftFn { &shiftLanes<uint64_t, ShiftKind::RightSigned> } },
        { .opcode = Opcode::I64x2ShrU,
          .evaluator = ShiftFn { &shiftLanes<uint64_t, ShiftKind::RightUnsigned> } },
        // Integer arithmetic
        { .opcode = Opcode::I8x16Add, .evaluator = BinaryFn { &binaryLanes<uint8_t, Add> } },
        { .opcode = Opcode::I8x16Sub, .evaluator = BinaryFn { &binaryLanes<uint8_t, Sub> } },
        { .opcode = Opcode::I8x16Neg, .evaluator = UnaryFn { &unaryLanes<uint8_t, Neg> } },
        { .opcode = Opcode::I8x16Abs, .evaluator = UnaryFn { &unaryLanes<uint8_t, Abs> } },
        { .opcode = Opcode::I16x8Add, .evaluator = BinaryFn { &binaryLanes<uint16_t, Add> } },
        { .opcode = Opcode::I16x8Sub, .evaluator = BinaryFn { &binaryLanes<uint16_t, Sub> } },
        { .opcode = Opcode::I16x8Mul, .evaluator = BinaryFn { &binaryLanes<uint16_t, Mul> } },
        { .opcode = Opcode::I16x8Neg, .evaluator = UnaryFn { &unaryLanes<uint16_t, Neg> } },
        { .opcode = Opcode::I16x8Abs, .evaluator = UnaryFn { &unaryLanes<uint16_t, Abs> } },
        { .opcode = Opcode::I32x4Add, .evaluator = BinaryFn { &binaryLanes<uint32_t, Add> } },
        { .opcode = Opcode::I32x4Sub, .evaluator = BinaryFn { &binaryLanes<uint32_t, Sub> } },
        { .opcode = Opcode::I32x4Mul, .evaluator = BinaryFn { &binaryLanes<uint32_t, Mul> } },
        { .opcode = Opcode::I32x4Neg, .evaluator = UnaryFn { &unaryLanes<uint32_t, Neg> } },
        { .opcode = Opcode::I32x4Abs, .evaluator = UnaryFn { &unaryLanes<uint32_t, Abs> } },
        { .opcode = Opcode::I64x2Add, .evaluator = BinaryFn { &binaryLanes<uint64_t, Add> } },
        { .opcode = Opcode::I64x2Sub, .evaluator = BinaryFn { &binaryLanes<uint64_t, Sub> } },
        { .opcode = Opcode::I64x2Mul, .evaluator = BinaryFn { &binaryLanes<uint64_t, Mul> } },
        { .opcode = Opcode::I64x2Neg, .evaluator = UnaryFn { &unaryLanes<uint64_t, Neg> } },
        { .opcode = Opcode::I64x2Abs, .evaluator = UnaryFn { &unaryLanes<uint64_t, Abs> } },
        { .opcode = Opcode::I8x16Popcnt, .evaluator = UnaryFn { &unaryLanes<uint8_t, Popcnt> } },
        // Minimum, maximum and average
        { .opcode = Opcode::I8x16MinS,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Pick<std::less<>, Signed>> } },
        { .opcode = Opcode::I8x16MinU,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Pick<std::less<>, Unsigned>> } },
        { .opcode = Opcode::I8x16MaxS,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Pick<std::greater<>, Signed>> } },
        { .opcode = Opcode::I8x16MaxU,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Pick<std::greater<>, Unsigned>> } },
        { .opcode = Opcode::I16x8MinS,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Pick<std::less<>, Signed>> } },
        { .opcode = Opcode::I16x8MinU,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Pick<std::less<>, Unsigned>> } },
        { .opcode = Opcode::I16x8MaxS,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Pick<std::greater<>, Signed>> } },
        { .opcode = Opcode::I16x8MaxU,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Pick<std::greater<>, Unsigned>> } },
        { .opcode = Opcode::I32x4MinS,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Pick<std::less<>, Signed>> } },
        { .opcode = Opcode::I32x4MinU,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Pick<std::less<>, Unsigned>> } },
        { .opcode = Opcode::I32x4MaxS,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Pick<std::greater<>, Signed>> } },
        { .opcode = Opcode::I32x4MaxU,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Pick<std::greater<>, Unsigned>> } },
        { .opcode = Opcode::I8x16AvgrU, .evaluator = BinaryFn { &binaryLanes<uint8_t, AverageRounded> } },
        { .opcode = Opcode::I16x8AvgrU, .evaluator = BinaryFn { &binaryLanes<uint16_t, AverageRounded> } },
        // Saturating arithmetic
        { .opcode = Opcode::I8x16AddSatS,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Saturating<std::plus<>, Signed>> } },
        { .opcode = Opcode::I8x16AddSatU,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Saturating<std::plus<>, Unsigned>> } },
        { .opcode = Opcode::I8x16SubSatS,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Saturating<std::minus<>, Signed>> } },
        { .opcode = Opcode::I8x16SubSatU,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Saturating<std::minus<>, Unsigned>> } },
        { .opcode = Opcode::I16x8AddSatS,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Saturating<std::plus<>, Signed>> } },
        { .opcode = Opcode::I16x8AddSatU,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Saturating<std::plus<>, Unsigned>> } },
        { .opcode = Opcode::I16x8SubSatS,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Saturating<std::minus<>, Signed>> } },
        { .opcode = Opcode::I16x8SubSatU,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Saturating<std::minus<>, Unsigned>> } },
        { .opcode = Opcode::I16x8Q15mulrSatS,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Q15MulRoundSaturate> } },
        // Comparisons
        { .opcode = Opcode::I8x16Eq,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Compare<std::equal_to<>, Unsigned>> } },
        { .opcode = Opcode::I8x16Ne,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Compare<std::not_equal_to<>, Unsigned>> } },
        { .opcode = Opcode::I8x16LtS,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Compare<std::less<>, Signed>> } },
        { .opcode = Opcode::I8x16LtU,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Compare<std::less<>, Unsigned>> } },
        { .opcode = Opcode::I8x16GtS,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Compare<std::greater<>, Signed>> } },
        { .opcode = Opcode::I8x16GtU,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Compare<std::greater<>, Unsigned>> } },
        { .opcode = Opcode::I8x16LeS,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Compare<std::less_equal<>, Signed>> } },
        { .opcode = Opcode::I8x16LeU,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Compare<std::less_equal<>, Unsigned>> } },
        { .opcode = Opcode::I8x16GeS,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Compare<std::greater_equal<>, Signed>> } },
        { .opcode = Opcode::I8x16GeU,
          .evaluator = BinaryFn { &binaryLanes<uint8_t, Compare<std::greater_equal<>, Unsigned>> } },
        { .opcode = Opcode::I16x8Eq,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Compare<std::equal_to<>, Unsigned>> } },
        { .opcode = Opcode::I16x8Ne,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Compare<std::not_equal_to<>, Unsigned>> } },
        { .opcode = Opcode::I16x8LtS,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Compare<std::less<>, Signed>> } },
        { .opcode = Opcode::I16x8LtU,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Compare<std::less<>, Unsigned>> } },
        { .opcode = Opcode::I16x8GtS,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Compare<std::greater<>, Signed>> } },
        { .opcode = Opcode::I16x8GtU,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Compare<std::greater<>, Unsigned>> } },
        { .opcode = Opcode::I16x8LeS,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Compare<std::less_equal<>, Signed>> } },
        { .opcode = Opcode::I16x8LeU,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Compare<std::less_equal<>, Unsigned>> } },
        { .opcode = Opcode::I16x8GeS,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Compare<std::greater_equal<>, Signed>> } },
        { .opcode = Opcode::I16x8GeU,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Compare<std::greater_equal<>, Unsigned>> } },
        { .opcode = Opcode::I32x4Eq,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Compare<std::equal_to<>, Unsigned>> } },
        { .opcode = Opcode::I32x4Ne,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Compare<std::not_equal_to<>, Unsigned>> } },
        { .opcode = Opcode::I32x4LtS,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Compare<std::less<>, Signed>> } },
        { .opcode = Opcode::I32x4LtU,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Compare<std::less<>, Unsigned>> } },
        { .opcode = Opcode::I32x4GtS,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Compare<std::greater<>, Signed>> } },
        { .opcode = Opcode::I32x4GtU,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Compare<std::greater<>, Unsigned>> } },
        { .opcode = Opcode::I32x4LeS,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Compare<std::less_equal<>, Signed>> } },
        { .opcode = Opcode::I32x4LeU,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Compare<std::less_equal<>, Unsigned>> } },
        { .opcode = Opcode::I32x4GeS,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Compare<std::greater_equal<>, Signed>> } },
        { .opcode = Opcode::I32x4GeU,
          .evaluator = BinaryFn { &binaryLanes<uint32_t, Compare<std::greater_equal<>, Unsigned>> } },
        { .opcode = Opcode::I64x2Eq,
          .evaluator = BinaryFn { &binaryLanes<uint64_t, Compare<std::equal_to<>, Unsigned>> } },
        { .opcode = Opcode::I64x2Ne,
          .evaluator = BinaryFn { &binaryLanes<uint64_t, Compare<std::not_equal_to<>, Unsigned>> } },
        { .opcode = Opcode::I64x2LtS,
          .evaluator = BinaryFn { &binaryLanes<uint64_t, Compare<std::less<>, Signed>> } },
        { .opcode = Opcode::I64x2GtS,
          .evaluator = BinaryFn { &binaryLanes<uint64_t, Compare<std::greater<>, Signed>> } },
        { .opcode = Opcode::I64x2LeS,
          .evaluator = BinaryFn { &binaryLanes<uint64_t, Compare<std::less_equal<>, Signed>> } },
        { .opcode = Opcode::I64x2GeS,
          .evaluator = BinaryFn { &binaryLanes<uint64_t, Compare<std::greater_equal<>, Signed>> } },
        // Lane tests
        { .opcode = Opcode::I8x16AllTrue, .evaluator = TestFn { &allTrue<uint8_t> } },
        { .opcode = Opcode::I8x16Bitmask, .evaluator = TestFn { &bitmask<uint8_t> } },
        { .opcode = Opcode::I16x8AllTrue, .evaluator = TestFn { &allTrue<uint16_t> } },
        { .opcode = Opcode::I16x8Bitmask, .evaluator = TestFn { &bitmask<uint16_t> } },
        { .opcode = Opcode::I32x4AllTrue, .evaluator = TestFn { &allTrue<uint32_t> } },
        { .opcode = Opcode::I32x4Bitmask, .evaluator = TestFn { &bitmask<uint32_t> } },
        { .opcode = Opcode::I64x2AllTrue, .evaluator = TestFn { &allTrue<uint64_t> } },
        { .opcode = Opcode::I64x2Bitmask, .evaluator = TestFn { &bitmask<uint64_t> } },
        // Widening and narrowing
        { .opcode = Opcode::I16x8ExtendLowI8x16S,
          .evaluator = UnaryFn { &extendLanes<uint8_t, uint16_t, Half::Low, Signed> } },
        { .opcode = Opcode::I16x8ExtendLowI8x16U,
          .evaluator = UnaryFn { &extendLanes<uint8_t, uint16_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I16x8ExtendHighI8x16S,
          .evaluator = UnaryFn { &extendLanes<uint8_t, uint16_t, Half::High, Signed> } },
        { .opcode = Opcode::I16x8ExtendHighI8x16U,
          .evaluator = UnaryFn { &extendLanes<uint8_t, uint16_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I32x4ExtendLowI16x8S,
          .evaluator = UnaryFn { &extendLanes<uint16_t, uint32_t, Half::Low, Signed> } },
        { .opcode = Opcode::I32x4ExtendLowI16x8U,
          .evaluator = UnaryFn { &extendLanes<uint16_t, uint32_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I32x4ExtendHighI16x8S,
          .evaluator = UnaryFn { &extendLanes<uint16_t, uint32_t, Half::High, Signed> } },
        { .opcode = Opcode::I32x4ExtendHighI16x8U,
          .evaluator = UnaryFn { &extendLanes<uint16_t, uint32_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I64x2ExtendLowI32x4S,
          .evaluator = UnaryFn { &extendLanes<uint32_t, uint64_t, Half::Low, Signed> } },
        { .opcode = Opcode::I64x2ExtendLowI32x4U,
          .evaluator = UnaryFn { &extendLanes<uint32_t, uint64_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I64x2ExtendHighI32x4S,
          .evaluator = UnaryFn { &extendLanes<uint32_t, uint64_t, Half::High, Signed> } },
        { .opcode = Opcode::I64x2ExtendHighI32x4U,
          .evaluator = UnaryFn { &extendLanes<uint32_t, uint64_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I16x8ExtmulLowI8x16S,
          .evaluator = BinaryFn { &extendMultiply<uint8_t, uint16_t, Half::Low, Signed> } },
        { .opcode = Opcode::I16x8ExtmulLowI8x16U,
          .evaluator = BinaryFn { &extendMultiply<uint8_t, uint16_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I16x8ExtmulHighI8x16S,
          .evaluator = BinaryFn { &extendMultiply<uint8_t, uint16_t, Half::High, Signed> } },
        { .opcode = Opcode::I16x8ExtmulHighI8x16U,
          .evaluator = BinaryFn { &extendMultiply<uint8_t, uint16_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I32x4ExtmulLowI16x8S,
          .evaluator = BinaryFn { &extendMultiply<uint16_t, uint32_t, Half::Low, Signed> } },
        { .opcode = Opcode::I32x4ExtmulLowI16x8U,
          .evaluator = BinaryFn { &extendMultiply<uint16_t, uint32_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I32x4ExtmulHighI16x8S,
          .evaluator = BinaryFn { &extendMultiply<uint16_t, uint32_t, Half::High, Signed> } },
        { .opcode = Opcode::I32x4ExtmulHighI16x8U,
          .evaluator = BinaryFn { &extendMultiply<uint16_t, uint32_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I64x2ExtmulLowI32x4S,
          .evaluator = BinaryFn { &extendMultiply<uint32_t, uint64_t, Half::Low, Signed> } },
        { .opcode = Opcode::I64x2ExtmulLowI32x4U,
          .evaluator = BinaryFn { &extendMultiply<uint32_t, uint64_t, Half::Low, Unsigned> } },
        { .opcode = Opcode::I64x2ExtmulHighI32x4S,
          .evaluator = BinaryFn { &extendMultiply<uint32_t, uint64_t, Half::High, Signed> } },
        { .opcode = Opcode::I64x2ExtmulHighI32x4U,
          .evaluator = BinaryFn { &extendMultiply<uint32_t, uint64_t, Half::High, Unsigned> } },
        { .opcode = Opcode::I16x8ExtaddPairwiseI8x16S,
          .evaluator = UnaryFn { &extendAddPairwise<uint8_t, uint16_t, Signed> } },
        { .opcode = Opcode::I16x8ExtaddPairwiseI8x16U,
          .evaluator = UnaryFn { &extendAddPairwise<uint8_t, uint16_t, Unsigned> } },
        { .opcode = Opcode::I32x4ExtaddPairwiseI16x8S,
          .evaluator = UnaryFn { &extendAddPairwise<uint16_t, uint32_t, Signed> } },
        { .opcode = Opcode::I32x4ExtaddPairwiseI16x8U,
          .evaluator = UnaryFn { &extendAddPairwise<uint16_t, uint32_t, Unsigned> } },
        { .opcode = Opcode::I32x4DotI16x8S, .evaluator = BinaryFn { &dot } },
        { .opcode = Opcode::I8x16NarrowI16x8S,
          .evaluator = BinaryFn { &narrowLanes<uint16_t, uint8_t, Signed> } },
        { .opcode = Opcode::I8x16NarrowI16x8U,
          .evaluator = BinaryFn { &narrowLanes<uint16_t, uint8_t, Unsigned> } },
        { .opcode = Opcode::I16x8NarrowI32x4S,
          .evaluator = BinaryFn { &narrowLanes<uint32_t, uint16_t, Signed> } },
        { .opcode = Opcode::I16x8NarrowI32x4U,
          .evaluator = BinaryFn { &narrowLanes<uint32_t, uint16_t, Unsigned> } },
        // Float arithmetic
        { .opcode = Opcode::F32x4Abs, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F32Abs, F32> } },
        { .opcode = Opcode::F32x4Neg, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F32Neg, F32> } },
        { .opcode = Opcode::F32x4Sqrt, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F32Sqrt, F32> } },
        { .opcode = Opcode::F32x4Add, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F32Add, F32> } },
        { .opcode = Opcode::F32x4Sub, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F32Sub, F32> } },
        { .opcode = Opcode::F32x4Mul, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F32Mul, F32> } },
        { .opcode = Opcode::F32x4Div, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F32Div, F32> } },
        { .opcode = Opcode::F32x4Min, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F32Min, F32> } },
        { .opcode = Opcode::F32x4Max, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F32Max, F32> } },
        { .opcode = Opcode::F32x4Pmin,
          .evaluator = BinaryFn { &pseudoLanes<Pseudo::Minimum, Opcode::F32Lt, F32> } },
        { .opcode = Opcode::F32x4Pmax,
          .evaluator = BinaryFn { &pseudoLanes<Pseudo::Maximum, Opcode::F32Lt, F32> } },
        { .opcode = Opcode::F64x2Abs, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F64Abs, F64> } },
        { .opcode = Opcode::F64x2Neg, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F64Neg, F64> } },
        { .opcode = Opcode::F64x2Sqrt, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F64Sqrt, F64> } },
        { .opcode = Opcode::F64x2Add, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F64Add, F64> } },
        { .opcode = Opcode::F64x2Sub, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F64Sub, F64> } },
        { .opcode = Opcode::F64x2Mul, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F64Mul, F64> } },
        { .opcode = Opcode::F64x2Div, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F64Div, F64> } },
        { .opcode = Opcode::F64x2Min, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F64Min, F64> } },
        { .opcode = Opcode::F64x2Max, .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F64Max, F64> } },
        { .opcode = Opcode::F64x2Pmin,
          .evaluator = BinaryFn { &pseudoLanes<Pseudo::Minimum, Opcode::F64Lt, F64> } },
        { .opcode = Opcode::F64x2Pmax,
          .evaluator = BinaryFn { &pseudoLanes<Pseudo::Maximum, Opcode::F64Lt, F64> } },
        // Rounding to an integral value
        { .opcode = Opcode::F32x4Ceil, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F32Ceil, F32> } },
        { .opcode = Opcode::F32x4Floor, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F32Floor, F32> } },
        { .opcode = Opcode::F32x4Trunc, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F32Trunc, F32> } },
        { .opcode = Opcode::F32x4Nearest,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F32Nearest, F32> } },
        { .opcode = Opcode::F64x2Ceil, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F64Ceil, F64> } },
        { .opcode = Opcode::F64x2Floor, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F64Floor, F64> } },
        { .opcode = Opcode::F64x2Trunc, .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F64Trunc, F64> } },
        { .opcode = Opcode::F64x2Nearest,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F64Nearest, F64> } },
        // Float comparisons
        { .opcode = Opcode::F32x4Eq, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F32Eq, F32> } },
        { .opcode = Opcode::F32x4Ne, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F32Ne, F32> } },
        { .opcode = Opcode::F32x4Lt, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F32Lt, F32> } },
        { .opcode = Opcode::F32x4Gt, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F32Gt, F32> } },
        { .opcode = Opcode::F32x4Le, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F32Le, F32> } },
        { .opcode = Opcode::F32x4Ge, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F32Ge, F32> } },
        { .opcode = Opcode::F64x2Eq, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F64Eq, F64> } },
        { .opcode = Opcode::F64x2Ne, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F64Ne, F64> } },
        { .opcode = Opcode::F64x2Lt, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F64Lt, F64> } },
        { .opcode = Opcode::F64x2Gt, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F64Gt, F64> } },
        { .opcode = Opcode::F64x2Le, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F64Le, F64> } },
        { .opcode = Opcode::F64x2Ge, .evaluator = BinaryFn { &compareScalarLanes<Opcode::F64Ge, F64> } },
        // Conversions
        { .opcode = Opcode::I32x4TruncSatF32x4S,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::I32TruncSatF32S, F32, I32> } },
        { .opcode = Opcode::I32x4TruncSatF32x4U,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::I32TruncSatF32U, F32, I32> } },
        { .opcode = Opcode::I32x4TruncSatF64x2SZero,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::I32TruncSatF64S, F64, I32> } },
        { .opcode = Opcode::I32x4TruncSatF64x2UZero,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::I32TruncSatF64U, F64, I32> } },
        { .opcode = Opcode::F32x4ConvertI32x4S,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F32ConvertI32S, I32, F32> } },
        { .opcode = Opcode::F32x4ConvertI32x4U,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F32ConvertI32U, I32, F32> } },
        { .opcode = Opcode::F64x2ConvertLowI32x4S,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F64ConvertI32S, I32, F64> } },
        { .opcode = Opcode::F64x2ConvertLowI32x4U,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F64ConvertI32U, I32, F64> } },
        { .opcode = Opcode::F32x4DemoteF64x2Zero,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F32DemoteF64, F64, F32> } },
        { .opcode = Opcode::F64x2PromoteLowF32x4,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::F64PromoteF32, F32, F64> } },
        // Relaxed SIMD, the Wasmtime-deterministic profile (Simd.hpp)
        { .opcode = Opcode::I8x16RelaxedSwizzle, .evaluator = BinaryFn { &swizzle } },
        { .opcode = Opcode::I32x4RelaxedTruncF32x4S,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::I32TruncSatF32S, F32, I32> } },
        { .opcode = Opcode::I32x4RelaxedTruncF32x4U,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::I32TruncSatF32U, F32, I32> } },
        { .opcode = Opcode::I32x4RelaxedTruncF64x2SZero,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::I32TruncSatF64S, F64, I32> } },
        { .opcode = Opcode::I32x4RelaxedTruncF64x2UZero,
          .evaluator = UnaryFn { &unaryScalarLanes<Opcode::I32TruncSatF64U, F64, I32> } },
        { .opcode = Opcode::F32x4RelaxedMadd,
          .evaluator = TernaryFn { &fusedMultiplyAdd<F32, ProductSign::Positive> } },
        { .opcode = Opcode::F32x4RelaxedNmadd,
          .evaluator = TernaryFn { &fusedMultiplyAdd<F32, ProductSign::Negated> } },
        { .opcode = Opcode::F64x2RelaxedMadd,
          .evaluator = TernaryFn { &fusedMultiplyAdd<F64, ProductSign::Positive> } },
        { .opcode = Opcode::F64x2RelaxedNmadd,
          .evaluator = TernaryFn { &fusedMultiplyAdd<F64, ProductSign::Negated> } },
        { .opcode = Opcode::I8x16RelaxedLaneselect, .evaluator = TernaryFn { &bitSelect } },
        { .opcode = Opcode::I16x8RelaxedLaneselect, .evaluator = TernaryFn { &bitSelect } },
        { .opcode = Opcode::I32x4RelaxedLaneselect, .evaluator = TernaryFn { &bitSelect } },
        { .opcode = Opcode::I64x2RelaxedLaneselect, .evaluator = TernaryFn { &bitSelect } },
        { .opcode = Opcode::F32x4RelaxedMin,
          .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F32Min, F32> } },
        { .opcode = Opcode::F32x4RelaxedMax,
          .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F32Max, F32> } },
        { .opcode = Opcode::F64x2RelaxedMin,
          .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F64Min, F64> } },
        { .opcode = Opcode::F64x2RelaxedMax,
          .evaluator = BinaryFn { &binaryScalarLanes<Opcode::F64Max, F64> } },
        { .opcode = Opcode::I16x8RelaxedQ15mulrS,
          .evaluator = BinaryFn { &binaryLanes<uint16_t, Q15MulRoundSaturate> } },
        { .opcode = Opcode::I16x8RelaxedDotI8x16I7x16S, .evaluator = BinaryFn { &relaxedDot } },
        { .opcode = Opcode::I32x4RelaxedDotI8x16I7x16AddS, .evaluator = TernaryFn { &relaxedDotAdd } },
        // Splats
        { .opcode = Opcode::I8x16Splat, .evaluator = SplatFn { &splatOf<uint8_t, I32> } },
        { .opcode = Opcode::I16x8Splat, .evaluator = SplatFn { &splatOf<uint16_t, I32> } },
        { .opcode = Opcode::I32x4Splat, .evaluator = SplatFn { &splatOf<uint32_t, I32> } },
        { .opcode = Opcode::I64x2Splat, .evaluator = SplatFn { &splatOf<uint64_t, I64> } },
        { .opcode = Opcode::F32x4Splat, .evaluator = SplatFn { &splatOf<uint32_t, F32> } },
        { .opcode = Opcode::F64x2Splat, .evaluator = SplatFn { &splatOf<uint64_t, F64> } },
        // Lanes
        { .opcode = Opcode::I8x16ExtractLaneS,
          .evaluator = ExtractFn { &extractOf<uint8_t, I32, Extension::Sign> } },
        { .opcode = Opcode::I8x16ExtractLaneU, .evaluator = ExtractFn { &extractOf<uint8_t, I32> } },
        { .opcode = Opcode::I16x8ExtractLaneS,
          .evaluator = ExtractFn { &extractOf<uint16_t, I32, Extension::Sign> } },
        { .opcode = Opcode::I16x8ExtractLaneU, .evaluator = ExtractFn { &extractOf<uint16_t, I32> } },
        { .opcode = Opcode::I32x4ExtractLane, .evaluator = ExtractFn { &extractOf<uint32_t, I32> } },
        { .opcode = Opcode::I64x2ExtractLane, .evaluator = ExtractFn { &extractOf<uint64_t, I64> } },
        { .opcode = Opcode::F32x4ExtractLane, .evaluator = ExtractFn { &extractOf<uint32_t, F32> } },
        { .opcode = Opcode::F64x2ExtractLane, .evaluator = ExtractFn { &extractOf<uint64_t, F64> } },
        { .opcode = Opcode::I8x16ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint8_t, I32> } },
        { .opcode = Opcode::I16x8ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint16_t, I32> } },
        { .opcode = Opcode::I32x4ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint32_t, I32> } },
        { .opcode = Opcode::I64x2ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint64_t, I64> } },
        { .opcode = Opcode::F32x4ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint32_t, F32> } },
        { .opcode = Opcode::F64x2ReplaceLane, .evaluator = ReplaceFn { &replaceOf<uint64_t, F64> } },
    });

    /// Each opcode's evaluator, by opcode; monostate for an opcode without a row.
    constexpr auto SimdByOpcode = [] {
        auto table = std::array<Evaluator, binary::OpcodeCount> {};
        for (auto const& row: SimdTable)
            table.at(static_cast<std::size_t>(row.opcode)) = row.evaluator;
        return table;
    }();

    /// The evaluator of @p opcode, of type @p Fn. Precondition (checked): its row holds one, that
    /// is, the opcode has the shape @p Fn stands for.
    template <typename Fn>
    [[nodiscard]] Fn evaluatorOf(Opcode opcode)
    {
        auto const* const evaluator = std::get_if<Fn>(&SimdByOpcode[static_cast<std::size_t>(opcode)]);
        Require(evaluator != nullptr);
        return *evaluator;
    }
} // namespace

SimdShape simdShape(Opcode opcode) noexcept
{
    return static_cast<SimdShape>(SimdByOpcode[static_cast<std::size_t>(opcode)].index());
}

V128 applySimdUnary(Opcode opcode, V128 operand)
{
    return evaluatorOf<UnaryFn>(opcode)(operand);
}

V128 applySimdBinary(Opcode opcode, V128 lhs, V128 rhs)
{
    return evaluatorOf<BinaryFn>(opcode)(lhs, rhs);
}

V128 applySimdTernary(Opcode opcode, V128 first, V128 second, V128 third)
{
    return evaluatorOf<TernaryFn>(opcode)(first, second, third);
}

I32 applySimdTest(Opcode opcode, V128 operand)
{
    return evaluatorOf<TestFn>(opcode)(operand);
}

V128 applySimdShift(Opcode opcode, V128 operand, uint32_t count)
{
    return evaluatorOf<ShiftFn>(opcode)(operand, count);
}

V128 splat(Opcode opcode, Value const& value)
{
    return evaluatorOf<SplatFn>(opcode)(value);
}

Value extractLane(Opcode opcode, V128 vector, uint8_t lane)
{
    return evaluatorOf<ExtractFn>(opcode)(vector, lane);
}

V128 replaceLane(Opcode opcode, V128 vector, uint8_t lane, Value const& value)
{
    return evaluatorOf<ReplaceFn>(opcode)(vector, lane, value);
}

V128 shuffle(V128 a, V128 b, std::span<std::byte const, 16> lanes) noexcept
{
    auto result = V128 {};
    for (auto const index: std::views::iota(0U, LaneCount<uint8_t>))
    {
        auto const lane = std::to_integer<unsigned>(lanes[index]);
        Require(lane < 2 * LaneCount<uint8_t>);
        auto const& source = lane < LaneCount<uint8_t> ? a : b;
        setLane<uint8_t>(result, index, laneOf<uint8_t>(source, lane % LaneCount<uint8_t>));
    }
    return result;
}

} // namespace isoline::interpreter
