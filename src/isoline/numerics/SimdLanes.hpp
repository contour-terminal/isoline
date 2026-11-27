// SPDX-License-Identifier: Apache-2.0
#pragma once

// A v128's lanes, and the templates that lift a scalar kernel to every lane of a vector: no boxed
// runtime::Value, no std::expected and no check per lane, so that an executor that inlines one
// (the fast interpreter's dedicated rows) pays for the lanes' arithmetic alone.
//
// A lane type is either an unsigned integer of the lane's width (uint8_t to uint64_t), for the
// integer lane kernels below, or a scalar value type (runtime::I32, I64, F32, F64), for the scalar
// kernels of Integer.hpp and Float.hpp. Lanes are numbered as WebAssembly numbers them,
// little-endian: lane 0 of an i32x4 is the low 32 bits of the low half, whatever the host's byte
// order, as V128 holds its halves as integers.

#include <core/Assert.hpp>

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <type_traits>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::numerics
{

// --- Lanes --------------------------------------------------------------------------------

/// The number of lanes of type @p Lane a v128 holds.
template <std::unsigned_integral Lane>
inline constexpr auto LaneCount = unsigned { 16 / sizeof(Lane) };

/// The bit width of a lane of type @p Lane.
template <std::unsigned_integral Lane>
inline constexpr auto LaneBits = unsigned { std::numeric_limits<Lane>::digits };

namespace detail
{
    /// The bits of lane @p index of @p vector, unchecked: callers iterate below LaneCount<Lane>.
    template <std::unsigned_integral Lane>
    [[nodiscard]] constexpr Lane laneAt(runtime::V128 const& vector, unsigned index) noexcept
    {
        constexpr auto PerHalf = LaneCount<Lane> / 2;
        auto const shift = (index % PerHalf) * LaneBits<Lane>;
        return static_cast<Lane>(vector.lanes[index / PerHalf] >> shift);
    }

    /// Sets lane @p index of @p vector to @p value, unchecked: callers iterate below LaneCount<Lane>.
    template <std::unsigned_integral Lane>
    constexpr void putLane(runtime::V128& vector, unsigned index, Lane value) noexcept
    {
        constexpr auto PerHalf = LaneCount<Lane> / 2;
        auto const shift = (index % PerHalf) * LaneBits<Lane>;
        auto const mask = uint64_t { std::numeric_limits<Lane>::max() } << shift;
        auto& half = vector.lanes[index / PerHalf];
        half = (half & ~mask) | (uint64_t { value } << shift);
    }
} // namespace detail

/// Lane @p index of @p v, read as an unsigned @p Lane; lane 0 is the vector's lowest bits.
/// Precondition (checked): @p index is below LaneCount<Lane>.
/// @param v The vector.
/// @param index The lane.
/// @return The lane's bits.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr Lane laneOf(runtime::V128 const& v, unsigned index) noexcept
{
    Require(index < LaneCount<Lane>);
    return detail::laneAt<Lane>(v, index);
}

/// Sets lane @p index of @p v to @p value, leaving every other lane as it is.
/// Precondition (checked): @p index is below LaneCount<Lane>.
/// @param v The vector to change.
/// @param index The lane.
/// @param value The lane's new bits.
template <std::unsigned_integral Lane>
constexpr void setLane(runtime::V128& v, unsigned index, Lane value) noexcept
{
    Require(index < LaneCount<Lane>);
    detail::putLane<Lane>(v, index, value);
}

// --- Lane types ---------------------------------------------------------------------------

/// A lane type: an unsigned integer of the lane's width, or a scalar value type, whose bits are a
/// lane's.
template <typename Lane>
concept SimdLane = std::unsigned_integral<Lane> || std::unsigned_integral<decltype(Lane {}.bits)>;

namespace detail
{
    template <SimdLane Lane>
    struct LaneRepresentation
    {
        using Bits = Lane;
    };

    template <SimdLane Lane>
        requires(!std::unsigned_integral<Lane>)
    struct LaneRepresentation<Lane>
    {
        using Bits = decltype(Lane {}.bits);
    };
} // namespace detail

/// The unsigned integer holding the bits of a lane of type @p Lane.
template <SimdLane Lane>
using LaneBitsOf = detail::LaneRepresentation<Lane>::Bits;

namespace detail
{
    /// The lane of type @p Lane whose bits are @p bits.
    template <SimdLane Lane>
    [[nodiscard]] constexpr Lane asLane(LaneBitsOf<Lane> bits) noexcept
    {
        if constexpr (std::unsigned_integral<Lane>)
            return bits;
        else
            return Lane { bits };
    }

    /// A scalar kernel's result as a lane of @p Bits: a truth value is all ones or zero, as SIMD
    /// comparisons give, an integer its bits, a value type the bits it holds.
    template <std::unsigned_integral Bits, typename Result>
    [[nodiscard]] constexpr Bits resultBits(Result result) noexcept
    {
        if constexpr (std::same_as<Result, bool>)
            return result ? std::numeric_limits<Bits>::max() : Bits { 0 };
        else if constexpr (std::unsigned_integral<Result>)
        {
            static_assert(sizeof(Result) == sizeof(Bits), "a lane kernel keeps the lane's width");
            return result;
        }
        else
        {
            static_assert(sizeof(result.bits) == sizeof(Bits), "a lane kernel keeps the lane's width");
            return result.bits;
        }
    }

    /// The vector whose lane i, of @p Bits, is @p function of lane i of each of @p operands.
    ///
    /// Each half is assembled in a register from its lanes' results, with no per-lane
    /// read-modify-write of the result and no check: the loop bounds are the lane counts. Lanes are
    /// read from and written to the halves' integer values, so the host's byte order never shows.
    template <std::unsigned_integral Bits, typename Function, std::same_as<runtime::V128>... Operands>
    [[nodiscard]] constexpr runtime::V128 lanewise(Function function, Operands const&... operands) noexcept
    {
        constexpr auto PerHalf = LaneCount<Bits> / 2;
        auto result = runtime::V128 {};
        for (auto const half: std::views::iota(std::size_t { 0 }, result.lanes.size()))
        {
            auto word = uint64_t { 0 };
            for (auto const index: std::views::iota(0U, PerHalf))
            {
                auto const shift = index * LaneBits<Bits>;
                auto const lane = Bits { function(static_cast<Bits>(operands.lanes[half] >> shift)...) };
                word |= uint64_t { lane } << shift;
            }
            result.lanes[half] = word;
        }
        return result;
    }
} // namespace detail

/// The vector whose every lane is @p Operation of that lane of @p operand, read as a @p Lane.
/// @tparam Lane The lane type @p Operation takes.
/// @tparam Operation A scalar kernel: a lane type to a lane of the same width, or to a truth value.
/// @param operand The vector.
/// @return The vector of the results.
template <SimdLane Lane, auto Operation>
[[nodiscard]] constexpr runtime::V128 mapLanes(runtime::V128 operand) noexcept
{
    using Bits = LaneBitsOf<Lane>;
    return detail::lanewise<Bits>(
        [](Bits lane) { return detail::resultBits<Bits>(Operation(detail::asLane<Lane>(lane))); }, operand);
}

/// The vector whose every lane is @p Operation of that lane of @p lhs and of @p rhs.
/// @tparam Lane The lane type @p Operation takes.
/// @tparam Operation A scalar kernel: two lanes to a lane of their width, or to a truth value, all
///                   ones where it holds (the SIMD comparisons).
/// @param lhs The first operand.
/// @param rhs The second operand.
/// @return The vector of the results.
template <SimdLane Lane, auto Operation>
[[nodiscard]] constexpr runtime::V128 mapLanes(runtime::V128 lhs, runtime::V128 rhs) noexcept
{
    using Bits = LaneBitsOf<Lane>;
    return detail::lanewise<Bits>(
        [](Bits a, Bits b) {
            return detail::resultBits<Bits>(Operation(detail::asLane<Lane>(a), detail::asLane<Lane>(b)));
        },
        lhs,
        rhs);
}

/// The vector whose every lane is @p Operation of that lane of each of the three operands.
/// @tparam Lane The lane type @p Operation takes.
/// @tparam Operation A scalar kernel: three lanes to a lane of their width.
/// @param first The first operand.
/// @param second The second operand.
/// @param third The third operand.
/// @return The vector of the results.
template <SimdLane Lane, auto Operation>
[[nodiscard]] constexpr runtime::V128 mapLanes(runtime::V128 first,
                                               runtime::V128 second,
                                               runtime::V128 third) noexcept
{
    using Bits = LaneBitsOf<Lane>;
    return detail::lanewise<Bits>(
        [](Bits a, Bits b, Bits c) {
            return detail::resultBits<Bits>(
                Operation(detail::asLane<Lane>(a), detail::asLane<Lane>(b), detail::asLane<Lane>(c)));
        },
        first,
        second,
        third);
}

/// i8x16.shuffle: byte i of the result is byte lanes[i] of the 32 bytes of @p a, then @p b. Both executors
/// run it: its 16 lane indices are an immediate no operator kernel takes.
/// Precondition (checked): every lane index is below 32, as validation ensures.
/// @param a The first operand, bytes 0 to 15.
/// @param b The second operand, bytes 16 to 31.
/// @param lanes The 16 lane index immediates.
/// @return The shuffled vector.
[[nodiscard]] constexpr runtime::V128 shuffle(runtime::V128 a,
                                              runtime::V128 b,
                                              std::span<std::byte const, 16> lanes) noexcept
{
    auto result = runtime::V128 {};
    for (auto const index: std::views::iota(0U, LaneCount<uint8_t>))
    {
        auto const lane = std::to_integer<unsigned>(lanes[index]);
        Require(lane < 2 * LaneCount<uint8_t>);
        auto const& source = lane < LaneCount<uint8_t> ? a : b;
        detail::putLane<uint8_t>(result, index, detail::laneAt<uint8_t>(source, lane % LaneCount<uint8_t>));
    }
    return result;
}

// --- Integer lane kernels -----------------------------------------------------------------
//
// The scalar kernels of the integer lanes, for which Integer.hpp, whose types are 32 and 64 bits
// wide, has none: each takes and gives an unsigned lane, and wraps, compares or saturates at the
// lane's width.

/// The type a lane of @p Lane is computed in: at least unsigned int, so a narrow lane's sum or
/// product wraps instead of overflowing the int it would promote to.
template <std::unsigned_integral Lane>
using Promoted = decltype(Lane {} + 0U);

/// @p lane read as signed when @p Reading is Sign, else as unsigned.
template <binary::Extension Reading, std::unsigned_integral Lane>
[[nodiscard]] constexpr auto interpreted(Lane lane) noexcept
{
    if constexpr (Reading == binary::Extension::Sign)
        return static_cast<std::make_signed_t<Lane>>(lane);
    else
        return lane;
}

/// @p lane widened to @p Wide, sign- or zero-extended per @p Widening.
template <std::unsigned_integral Wide, binary::Extension Widening, std::unsigned_integral Lane>
[[nodiscard]] constexpr Wide widened(Lane lane) noexcept
{
    return static_cast<Wide>(static_cast<std::make_signed_t<Wide>>(interpreted<Widening>(lane)));
}

/// @p value clamped to the range of @p Lane read per @p Reading, as the lane's bits.
template <std::unsigned_integral Lane, binary::Extension Reading>
[[nodiscard]] constexpr Lane saturated(int64_t value) noexcept
{
    using Target = decltype(interpreted<Reading>(Lane {}));
    auto const low = int64_t { std::numeric_limits<Target>::min() };
    auto const high = int64_t { std::numeric_limits<Target>::max() };
    return static_cast<Lane>(std::clamp(value, low, high));
}

/// add: the sum, wrapping at the lane's width.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr Lane laneAdd(Lane lhs, Lane rhs) noexcept
{
    return static_cast<Lane>(Promoted<Lane> { lhs } + rhs);
}

/// sub: the difference, wrapping at the lane's width.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr Lane laneSub(Lane lhs, Lane rhs) noexcept
{
    return static_cast<Lane>(Promoted<Lane> { lhs } - rhs);
}

/// The top bit of every lane of type @p Lane in a 64-bit half.
template <std::unsigned_integral Lane>
inline constexpr auto LaneTopBits =
    uint64_t { ~uint64_t { 0 } / std::numeric_limits<Lane>::max() } << (LaneBits<Lane> - 1);

/// add of every lane of type @p Lane in a 64-bit half at once: the sums of each lane's bits below
/// its top one cannot carry into the next lane, and the top bit is the sum's, carry-less. Wrapping
/// at the lane's width, as laneAdd of each lane.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr uint64_t packedAdd(uint64_t lhs, uint64_t rhs) noexcept
{
    constexpr auto Top = LaneTopBits<Lane>;
    return ((lhs & ~Top) + (rhs & ~Top)) ^ ((lhs ^ rhs) & Top);
}

/// sub of every lane of type @p Lane in a 64-bit half at once, as laneSub of each lane: each lane's
/// top bit is set before the subtraction, so no borrow leaves a lane, and then corrected.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr uint64_t packedSub(uint64_t lhs, uint64_t rhs) noexcept
{
    constexpr auto Top = LaneTopBits<Lane>;
    return ((lhs | Top) - (rhs & ~Top)) ^ ((lhs ^ ~rhs) & Top);
}

/// The vector whose each 64-bit half is @p Operation of that half of @p lhs and of @p rhs: for a
/// packed kernel, which computes every lane of a half at once.
/// @param lhs The first operand.
/// @param rhs The second operand.
/// @return The vector of the results.
template <auto Operation>
[[nodiscard]] constexpr runtime::V128 mapHalves(runtime::V128 lhs, runtime::V128 rhs) noexcept
{
    return runtime::V128 { .lanes = { Operation(lhs.lanes[0], rhs.lanes[0]),
                                      Operation(lhs.lanes[1], rhs.lanes[1]) } };
}

/// mul: the low half of the product.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr Lane laneMul(Lane lhs, Lane rhs) noexcept
{
    return static_cast<Lane>(Promoted<Lane> { lhs } * rhs);
}

/// neg: the negation, wrapping at the lane's width.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr Lane laneNeg(Lane lane) noexcept
{
    return static_cast<Lane>(Promoted<Lane> { 0 } - lane);
}

/// abs: the absolute value, read as signed; the most negative lane is its own absolute value.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr Lane laneAbs(Lane lane) noexcept
{
    return interpreted<binary::Extension::Sign>(lane) < 0 ? laneNeg(lane) : lane;
}

/// popcnt: the number of set bits.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr Lane lanePopcnt(Lane lane) noexcept
{
    return static_cast<Lane>(std::popcount(lane));
}

/// min and max: the lane @p Relation prefers, comparing the lanes read per @p Reading; std::less
/// picks the minimum, std::greater the maximum.
template <std::unsigned_integral Lane, typename Relation, binary::Extension Reading>
[[nodiscard]] constexpr Lane lanePick(Lane lhs, Lane rhs) noexcept
{
    return Relation {}(interpreted<Reading>(lhs), interpreted<Reading>(rhs)) ? lhs : rhs;
}

/// A comparison of the lanes read per @p Reading; mapLanes makes it all ones where it holds.
template <std::unsigned_integral Lane, typename Relation, binary::Extension Reading>
[[nodiscard]] constexpr bool laneCompare(Lane lhs, Lane rhs) noexcept
{
    return Relation {}(interpreted<Reading>(lhs), interpreted<Reading>(rhs));
}

/// avgr_u: the unsigned average, rounded up.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr Lane laneAverageRounded(Lane lhs, Lane rhs) noexcept
{
    return static_cast<Lane>((Promoted<Lane> { lhs } + rhs + 1) / 2);
}

/// add_sat (@p Arithmetic std::plus) and sub_sat (std::minus), the lanes read per @p Reading. Only
/// i8 and i16 lanes saturate, so the exact result fits an int64_t.
template <std::unsigned_integral Lane, typename Arithmetic, binary::Extension Reading>
[[nodiscard]] constexpr Lane laneSaturating(Lane lhs, Lane rhs) noexcept
{
    static_assert(sizeof(Lane) <= 2);
    auto const exact =
        Arithmetic {}(int64_t { interpreted<Reading>(lhs) }, int64_t { interpreted<Reading>(rhs) });
    return saturated<Lane, Reading>(exact);
}

/// i16x8.q15mulr_sat_s: the Q15 product, rounded half up and saturated.
[[nodiscard]] constexpr uint16_t laneQ15MulRoundSaturate(uint16_t lhs, uint16_t rhs) noexcept
{
    auto const product =
        int64_t { interpreted<binary::Extension::Sign>(lhs) } * interpreted<binary::Extension::Sign>(rhs);
    return saturated<uint16_t, binary::Extension::Sign>((product + 0x4000) >> 15U);
}

/// Which way a lane-wise shift moves its bits.
enum class ShiftKind : uint8_t
{
    Left,          ///< shl.
    RightSigned,   ///< shr_s: the sign bit fills in.
    RightUnsigned, ///< shr_u: zeros fill in.
};

/// shl, shr_s and shr_u of one lane by @p distance, already below the lane's width.
template <std::unsigned_integral Lane, ShiftKind Kind>
[[nodiscard]] constexpr Lane laneShift(Lane lane, unsigned distance) noexcept
{
    if constexpr (Kind == ShiftKind::Left)
        return static_cast<Lane>(lane << distance);
    else if constexpr (Kind == ShiftKind::RightSigned)
        return static_cast<Lane>(interpreted<binary::Extension::Sign>(lane) >> distance);
    else
        return static_cast<Lane>(lane >> distance);
}

/// A lane-wise shift; the count is taken modulo the lane width, as the spec says.
/// @param operand The vector to shift.
/// @param count The shift count.
/// @return The shifted vector.
template <std::unsigned_integral Lane, ShiftKind Kind>
[[nodiscard]] constexpr runtime::V128 shiftLanes(runtime::V128 operand, uint32_t count) noexcept
{
    auto const distance = unsigned { count % LaneBits<Lane> };
    return detail::lanewise<Lane>([distance](Lane lane) { return laneShift<Lane, Kind>(lane, distance); },
                                  operand);
}

} // namespace isoline::numerics
