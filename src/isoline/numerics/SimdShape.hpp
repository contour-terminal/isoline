// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include <isoline/binary/Opcode.hpp>

namespace isoline::numerics
{

/// How a SIMD instruction takes its operands, and so which kernel getter of SimdKernels.hpp has its
/// kernel. The first five name a kernel each; the lane operations take a scalar or a lane index
/// immediate, which each executor handles with a dedicated row of its own.
enum class SimdShape : uint8_t
{
    None,        ///< Not a SIMD operator: a memory access, v128.const, i8x16.shuffle, or no SIMD row.
    Unary,       ///< One v128, one v128 result: simdUnaryKernel.
    Binary,      ///< Two v128, one v128 result: simdBinaryKernel.
    Ternary,     ///< Three v128, one v128 result: simdTernaryKernel.
    Test,        ///< One v128, an i32 result: simdTestKernel.
    Shift,       ///< A v128 and an i32 shift count, one v128 result: simdShiftKernel.
    Splat,       ///< One scalar, a v128 of copies of it.
    ExtractLane, ///< A v128 and a lane index immediate, the lane as a scalar.
    ReplaceLane, ///< A v128, a lane index immediate and a scalar, one v128 result.
};

/// How @p opcode takes its operands.
/// @param opcode Any instruction.
/// @return Its shape, or None when it has no row in the SIMD kernel table.
[[nodiscard]] SimdShape simdShape(binary::Opcode opcode) noexcept;

} // namespace isoline::numerics
