// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <isoline/binary/Opcode.hpp>
#include <isoline/runtime/Value.hpp>

// The SIMD operators of the reference interpreter, one row each in Simd.cpp's table.
//
// Cost: each operator is a table lookup by opcode, and each floating-point lane goes through the
// scalar operator of Numerics, boxed in a runtime::Value and returned in a std::expected. That is
// several times the work of the lane itself, which is fine for the reference interpreter, whose
// job is to be obviously right; the fast tiers lower these operators on their own.
//
// Relaxed SIMD: the Wasmtime-deterministic profile (Wasmtime v48.0.10, the version Contour pins).
//
// Each relaxed operator may give any result of a set the spec allows; Isoline gives one, always the
// same on every host: the one Wasmtime gives with Config::relaxed_simd_deterministic, as spec §11
// binds lane 1 to. A NaN result is the canonical NaN, as everywhere in Isoline. The sources are
// wasmtime crates/cranelift/src/translate/code_translator.rs (CT) and func_environ.rs (FE) at that
// tag.
//
//   relaxed_swizzle             i8x16.swizzle: an index of 16 or more gives 0      CT:2511, FE:5639
//   relaxed_trunc_f32x4_s       i32x4.trunc_sat_f32x4_s (NaN 0, saturating)        CT:2562
//   relaxed_trunc_f32x4_u       i32x4.trunc_sat_f32x4_u                            CT:2210
//   relaxed_trunc_f64x2_s_zero  i32x4.trunc_sat_f64x2_s_zero                       CT:2574
//   relaxed_trunc_f64x2_u_zero  i32x4.trunc_sat_f64x2_u_zero                       CT:2216
//   f32x4/f64x2.relaxed_madd    a * b + c, fused: rounded once                     CT:2517, FE:5679
//   f32x4/f64x2.relaxed_nmadd   -a * b + c, fused                                  CT:2527, FE:5700
//   *.relaxed_laneselect        v128.bitselect: bit by bit, not by each lane's top CT:2540
//   relaxed_min, relaxed_max    f32x4/f64x2.min and .max (-0 < +0, a NaN wins)     CT:2473, CT:2492
//   relaxed_q15mulr_s           i16x8.q15mulr_sat_s: 0x8000 * 0x8000 is 0x7FFF     CT:2590
//   relaxed_dot_i8x16_i7x16_s   both operands signed, the pair sum wrapping at 16
//                               bits: two products of -128 * -128 give 0x8000      CT:2604
//   relaxed_dot_..._add_s       both signed, the four products summed exactly in
//                               32 bits (no 16-bit intermediate), + c wrapping     CT:2623

namespace isoline::interp
{

/// How a SIMD instruction takes its operands, and so which apply function evaluates it.
enum class SimdShape : uint8_t
{
    None,        ///< Not a SIMD operator: a memory access, v128.const, i8x16.shuffle, or no SIMD row.
    Unary,       ///< One v128, one v128 result: applySimdUnary.
    Binary,      ///< Two v128, one v128 result: applySimdBinary.
    Ternary,     ///< Three v128, one v128 result: applySimdTernary.
    Test,        ///< One v128, an i32 result: applySimdTest.
    Shift,       ///< A v128 and an i32 shift count, one v128 result: applySimdShift.
    Splat,       ///< One scalar, a v128 of copies of it: splat.
    ExtractLane, ///< A v128 and a lane index immediate, the lane as a scalar: extractLane.
    ReplaceLane, ///< A v128, a lane index immediate and a scalar, one v128 result: replaceLane.
};

/// How @p opcode takes its operands.
/// @param opcode Any instruction.
/// @return Its shape, or None when it has no row in the SIMD operator table.
[[nodiscard]] SimdShape simdShape(binary::Opcode opcode) noexcept;

/// The number of lanes of type @p Lane a v128 holds.
template <std::unsigned_integral Lane>
inline constexpr auto LaneCount = unsigned { 16 / sizeof(Lane) };

/// Lane @p index of @p v, read as an unsigned @p Lane; lane 0 is the vector's lowest bits.
///
/// Lanes are numbered as WebAssembly numbers them, little-endian: lane 0 of an i32x4 is the low 32
/// bits of the low half, whatever the host's byte order.
/// Precondition (checked): @p index is below LaneCount<Lane>.
/// @param v The vector.
/// @param index The lane.
/// @return The lane's bits.
template <std::unsigned_integral Lane>
[[nodiscard]] constexpr Lane laneOf(runtime::V128 const& v, unsigned index) noexcept
{
    Require(index < LaneCount<Lane>);
    constexpr auto PerHalf = LaneCount<Lane> / 2;
    auto const shift = (index % PerHalf) * unsigned { std::numeric_limits<Lane>::digits };
    return static_cast<Lane>(v.lanes[index / PerHalf] >> shift);
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
    constexpr auto PerHalf = LaneCount<Lane> / 2;
    auto const shift = (index % PerHalf) * unsigned { std::numeric_limits<Lane>::digits };
    auto const mask = uint64_t { std::numeric_limits<Lane>::max() } << shift;
    auto& half = v.lanes[index / PerHalf];
    half = (half & ~mask) | (uint64_t { value } << shift);
}

/// Applies a unary vector operator: v128.not, or a lane-wise one (neg, abs, sqrt, rounding, an
/// extension or a conversion).
/// Precondition (checked): simdShape(opcode) is Unary.
/// @param opcode The instruction.
/// @param operand Its operand.
/// @return The result.
[[nodiscard]] runtime::V128 applySimdUnary(binary::Opcode opcode, runtime::V128 operand);

/// Applies a binary vector operator: a bitwise one, swizzle, or a lane-wise arithmetic operator or
/// comparison, integer or float, relaxed SIMD's binary operators included (see above).
/// Precondition (checked): simdShape(opcode) is Binary.
/// @param opcode The instruction.
/// @param lhs The first operand, the lower on the stack.
/// @param rhs The second operand, the top of the stack.
/// @return The result.
[[nodiscard]] runtime::V128 applySimdBinary(binary::Opcode opcode, runtime::V128 lhs, runtime::V128 rhs);

/// Applies a ternary vector operator: v128.bitselect, or relaxed SIMD's madd, nmadd, laneselect or
/// dot_add, in the Wasmtime-deterministic profile above.
/// Precondition (checked): simdShape(opcode) is Ternary.
/// @param opcode The instruction.
/// @param first The first operand, the lowest on the stack.
/// @param second The second operand.
/// @param third The third operand, the top of the stack.
/// @return The result.
[[nodiscard]] runtime::V128 applySimdTernary(binary::Opcode opcode,
                                             runtime::V128 first,
                                             runtime::V128 second,
                                             runtime::V128 third);

/// Applies a vector test (any_true, all_true, bitmask).
/// Precondition (checked): simdShape(opcode) is Test.
/// @param opcode The instruction.
/// @param operand Its operand.
/// @return The test's i32 result.
[[nodiscard]] runtime::I32 applySimdTest(binary::Opcode opcode, runtime::V128 operand);

/// Applies a lane-wise shift; the count is taken modulo the lane width, as the spec says.
/// Precondition (checked): simdShape(opcode) is Shift.
/// @param opcode The instruction.
/// @param operand The vector to shift.
/// @param count The shift count, the i32 on top of the stack.
/// @return The shifted vector.
[[nodiscard]] runtime::V128 applySimdShift(binary::Opcode opcode, runtime::V128 operand, uint32_t count);

/// The vector of copies of @p value in every lane of @p opcode's shape; a lane narrower than the
/// value takes its low bits.
/// Precondition (checked): simdShape(opcode) is Splat, and @p value is of its operand type.
/// @param opcode The instruction.
/// @param value The scalar to copy.
/// @return The vector.
[[nodiscard]] runtime::V128 splat(binary::Opcode opcode, runtime::Value const& value);

/// Lane @p lane of @p vector as the scalar @p opcode yields: an i8 or i16 lane sign- or
/// zero-extended to an i32, any other lane as its own type.
/// Precondition (checked): simdShape(opcode) is ExtractLane, and @p lane is below its lane count.
/// @param opcode The instruction.
/// @param vector The vector.
/// @param lane The lane index immediate.
/// @return The lane's value.
[[nodiscard]] runtime::Value extractLane(binary::Opcode opcode, runtime::V128 vector, uint8_t lane);

/// @p vector with lane @p lane set to @p value; a lane narrower than the value takes its low bits.
/// Precondition (checked): simdShape(opcode) is ReplaceLane, @p lane is below its lane count, and
/// @p value is of its operand type.
/// @param opcode The instruction.
/// @param vector The vector.
/// @param lane The lane index immediate.
/// @param value The lane's new value.
/// @return The changed vector.
[[nodiscard]] runtime::V128 replaceLane(binary::Opcode opcode,
                                        runtime::V128 vector,
                                        uint8_t lane,
                                        runtime::Value const& value);

/// i8x16.shuffle: byte i of the result is byte lanes[i] of the 32 bytes of @p a, then @p b.
/// Precondition (checked): every lane index is below 32, as validation ensures.
/// @param a The first operand, bytes 0 to 15.
/// @param b The second operand, bytes 16 to 31.
/// @param lanes The 16 lane index immediates.
/// @return The shuffled vector.
[[nodiscard]] runtime::V128 shuffle(runtime::V128 a,
                                    runtime::V128 b,
                                    std::span<std::byte const, 16> lanes) noexcept;

} // namespace isoline::interp
