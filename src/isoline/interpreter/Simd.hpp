// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <isoline/binary/Opcode.hpp>
#include <isoline/numerics/SimdLanes.hpp>
#include <isoline/numerics/SimdShape.hpp>
#include <isoline/runtime/Value.hpp>

// The SIMD operators of the reference interpreter: the shared kernels of numerics/SimdKernels.hpp,
// looked up by opcode, and the lane operations (splat, extract_lane, replace_lane) and
// i8x16.shuffle, which take a boxed scalar or an immediate, in Simd.cpp. The relaxed SIMD
// operators follow the Wasmtime-deterministic profile SimdKernels.hpp documents.
//
// Cost: an operator is a table lookup by opcode and a call of its kernel, which computes each lane
// with the typed scalar kernel of numerics, on bits; no lane is boxed in a runtime::Value.

namespace isoline::interpreter
{

/// How a SIMD instruction takes its operands; numerics owns it, as every executor shares it.
using numerics::SimdShape;
/// How an instruction takes its operands, SimdShape::None for every non-operator.
using numerics::simdShape;
/// The number of lanes of a lane type in a v128.
using numerics::LaneCount;
/// A lane of a v128, checked.
using numerics::laneOf;
/// Sets a lane of a v128, checked.
using numerics::setLane;

/// Applies a unary vector operator: numerics::simdUnaryKernel(opcode) on @p operand.
/// Precondition (checked): simdShape(opcode) is Unary.
/// @param opcode The instruction.
/// @param operand Its operand.
/// @return The result.
[[nodiscard]] runtime::V128 applySimdUnary(binary::Opcode opcode, runtime::V128 operand);

/// Applies a binary vector operator: numerics::simdBinaryKernel(opcode).
/// Precondition (checked): simdShape(opcode) is Binary.
/// @param opcode The instruction.
/// @param lhs The first operand, the lower on the stack.
/// @param rhs The second operand, the top of the stack.
/// @return The result.
[[nodiscard]] runtime::V128 applySimdBinary(binary::Opcode opcode, runtime::V128 lhs, runtime::V128 rhs);

/// Applies a ternary vector operator: numerics::simdTernaryKernel(opcode).
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

/// Applies a vector test (any_true, all_true, bitmask): numerics::simdTestKernel(opcode).
/// Precondition (checked): simdShape(opcode) is Test.
/// @param opcode The instruction.
/// @param operand Its operand.
/// @return The test's i32 result.
[[nodiscard]] runtime::I32 applySimdTest(binary::Opcode opcode, runtime::V128 operand);

/// Applies a lane-wise shift, numerics::simdShiftKernel(opcode); the count is taken modulo the lane
/// width, as the spec says.
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

} // namespace isoline::interpreter
