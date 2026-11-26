// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>

#include <isoline/binary/Opcode.hpp>
#include <isoline/numerics/CanonicalNan.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::interpreter
{

/// How a numeric instruction takes its operands, and so which apply function evaluates it.
enum class NumericShape : uint8_t
{
    None,    ///< Not a numeric instruction (or a constant, which takes no operand).
    Unary,   ///< One operand, one result of any type: applyUnary.
    Binary,  ///< Two operands of one type, one result of that type: applyBinary.
    Test,    ///< One operand, an i32 truth value (eqz): applyTest.
    Compare, ///< Two operands, an i32 truth value: applyCompare.
    Wide,    ///< Two or four i64 operands, a 128-bit result as two i64: applyWide.
};

/// How @p opcode takes its operands.
/// @param opcode Any instruction.
/// @return Its shape, or None when it is not a numeric instruction.
[[nodiscard]] NumericShape numericShape(binary::Opcode opcode) noexcept;

/// Applies a unary numeric operator (spec 4.3), conversions included.
///
/// Floats are operated on as bits wherever the spec allows no rounding: abs, neg, copysign,
/// reinterpret and the rounding to integral values (ceil, floor, trunc, nearest) never pass a value
/// through a floating-point register, so a NaN's payload is never touched by the host's FPU. Where
/// the spec's result is a NaN of the nans set (sqrt, the rounding operators, promote and demote of
/// a NaN), the result is the positive canonical NaN, a member of every such set.
/// Precondition (checked): numericShape(opcode) is Unary, and @p operand is of its operand type.
/// @param opcode The instruction.
/// @param operand Its operand.
/// @return The result, or the trap of a trapping conversion to an integer.
[[nodiscard]] std::expected<runtime::Value, runtime::Trap> applyUnary(binary::Opcode opcode,
                                                                      runtime::Value operand);

/// Applies a binary numeric operator (spec 4.3).
///
/// Float add, sub, mul and div are computed by the host in round-to-nearest; a NaN result becomes
/// the positive canonical NaN. They assume the default floating-point environment: round-to-nearest
/// set, and subnormals neither flushed to zero nor treated as zero (no FTZ or DAZ). Every invocation
/// of guest code establishes it (runtime::FloatEnvironmentScope, opened by runtime::InvocationScope),
/// and so does every return from a host function (runtime::HostCallScope); on targets without a
/// control-register row (runtime::hostFloatControlLayout()) only the rounding mode is pinned; the
/// embedder's side of it is docs/embedding.md's "Floating point". min and max
/// work on bits: any NaN operand gives the canonical NaN. Precondition (checked): numericShape(opcode) is
/// Binary, and both operands are of its type.
/// @param opcode The instruction.
/// @param lhs The first operand, the lower on the stack.
/// @param rhs The second operand, the top of the stack.
/// @return The result, or the trap of an integer division by zero or overflow.
[[nodiscard]] std::expected<runtime::Value, runtime::Trap> applyBinary(binary::Opcode opcode,
                                                                       runtime::Value lhs,
                                                                       runtime::Value rhs);

/// Applies a test operator: i32.eqz or i64.eqz.
/// Precondition (checked): numericShape(opcode) is Test, and @p operand is of its operand type.
/// @param opcode The instruction.
/// @param operand Its operand.
/// @return 1 when the test holds, else 0.
[[nodiscard]] runtime::I32 applyTest(binary::Opcode opcode, runtime::Value operand);

/// Applies a relational operator; a float comparison with a NaN operand is false, except ne.
/// Precondition (checked): numericShape(opcode) is Compare, and both operands are of its type.
/// @param opcode The instruction.
/// @param lhs The first operand, the lower on the stack.
/// @param rhs The second operand, the top of the stack.
/// @return 1 when the relation holds, else 0.
[[nodiscard]] runtime::I32 applyCompare(binary::Opcode opcode, runtime::Value lhs, runtime::Value rhs);

/// Applies a wide-arithmetic operator: i64.add128, i64.sub128, i64.mul_wide_s or i64.mul_wide_u.
///
/// A 128-bit value is two i64, its low half below its high half on the stack. The results are
/// modulo 2^128; none traps.
/// Precondition (checked): numericShape(opcode) is Wide, and @p operands holds as many as its
/// signature pops.
/// @param opcode The instruction.
/// @param operands Its operands' bits, bottom of the stack first: add128 and sub128 take
///                 {lhs low, lhs high, rhs low, rhs high}, the multiplications {lhs, rhs}.
/// @return The result's {low, high} halves, pushed in that order.
[[nodiscard]] std::array<uint64_t, 2> applyWide(binary::Opcode opcode,
                                                std::span<uint64_t const> operands) noexcept;

/// The canonical NaN of f32 (spec 4.3.3), which the float kernels produce.
using numerics::CanonicalNanF32;
/// The canonical NaN of f64, which the float kernels produce.
using numerics::CanonicalNanF64;

} // namespace isoline::interpreter
