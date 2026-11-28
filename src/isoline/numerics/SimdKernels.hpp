// SPDX-License-Identifier: Apache-2.0
#pragma once

// The SIMD operator kernels every executor evaluates vector instructions with, one per opcode, built
// on the typed scalar kernels of Integer.hpp and Float.hpp through SimdLanes.hpp's templates: each
// is a plain function of v128 operands, with no boxed runtime::Value, no std::expected and no check
// per lane. An executor resolves an opcode to its kernel once (the interpreter at lowering, the
// reference interpreter per instruction) and calls it.
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

#include <array>
#include <cstddef>
#include <cstdint>

#include <isoline/binary/Opcode.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::numerics
{

/// A unary vector operator: v128.not, or a lane-wise one (neg, abs, sqrt, rounding, an extension
/// or a conversion).
using SimdUnaryKernel = runtime::V128 (*)(runtime::V128) noexcept;

/// A binary vector operator: a bitwise one, swizzle, or a lane-wise arithmetic operator or
/// comparison, integer or float, relaxed SIMD's binary operators included; lhs is the lower operand
/// on the stack.
using SimdBinaryKernel = runtime::V128 (*)(runtime::V128, runtime::V128) noexcept;

/// A ternary vector operator: v128.bitselect, or relaxed SIMD's madd, nmadd, laneselect or dot_add;
/// the operands in stack order, the lowest first.
using SimdTernaryKernel = runtime::V128 (*)(runtime::V128, runtime::V128, runtime::V128) noexcept;

/// A vector test: any_true, all_true or bitmask, an i32 result.
using SimdTestKernel = runtime::I32 (*)(runtime::V128) noexcept;

/// A lane-wise shift by the i32 count on top of the stack, taken modulo the lane width.
using SimdShiftKernel = runtime::V128 (*)(runtime::V128, uint32_t) noexcept;

namespace detail
{
    /// Each opcode's kernel of a shape, by opcode, nullptr where it has another shape: what the
    /// getters below read, so that a lookup is one load at the call site. SimdKernels.cpp builds
    /// them at compile time from its one table of SIMD operators.
    extern std::array<SimdUnaryKernel, binary::OpcodeCount> const unaryKernels;
    extern std::array<SimdBinaryKernel, binary::OpcodeCount> const binaryKernels;   ///< As unaryKernels.
    extern std::array<SimdTernaryKernel, binary::OpcodeCount> const ternaryKernels; ///< As unaryKernels.
    extern std::array<SimdTestKernel, binary::OpcodeCount> const testKernels;       ///< As unaryKernels.
    extern std::array<SimdShiftKernel, binary::OpcodeCount> const shiftKernels;     ///< As unaryKernels.
} // namespace detail

/// The kernel of a unary vector operator.
/// @param opcode Any instruction.
/// @return Its kernel, or nullptr when simdShape(opcode) is not Unary.
[[nodiscard]] inline SimdUnaryKernel simdUnaryKernel(binary::Opcode opcode) noexcept
{
    return detail::unaryKernels[static_cast<std::size_t>(opcode)];
}

/// The kernel of a binary vector operator.
/// @param opcode Any instruction.
/// @return Its kernel, or nullptr when simdShape(opcode) is not Binary.
[[nodiscard]] inline SimdBinaryKernel simdBinaryKernel(binary::Opcode opcode) noexcept
{
    return detail::binaryKernels[static_cast<std::size_t>(opcode)];
}

/// The kernel of a ternary vector operator.
/// @param opcode Any instruction.
/// @return Its kernel, or nullptr when simdShape(opcode) is not Ternary.
[[nodiscard]] inline SimdTernaryKernel simdTernaryKernel(binary::Opcode opcode) noexcept
{
    return detail::ternaryKernels[static_cast<std::size_t>(opcode)];
}

/// The kernel of a vector test.
/// @param opcode Any instruction.
/// @return Its kernel, or nullptr when simdShape(opcode) is not Test.
[[nodiscard]] inline SimdTestKernel simdTestKernel(binary::Opcode opcode) noexcept
{
    return detail::testKernels[static_cast<std::size_t>(opcode)];
}

/// The kernel of a lane-wise shift.
/// @param opcode Any instruction.
/// @return Its kernel, or nullptr when simdShape(opcode) is not Shift.
[[nodiscard]] inline SimdShiftKernel simdShiftKernel(binary::Opcode opcode) noexcept
{
    return detail::shiftKernels[static_cast<std::size_t>(opcode)];
}

} // namespace isoline::numerics
