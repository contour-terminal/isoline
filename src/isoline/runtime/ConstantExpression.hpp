// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>

#include <isoline/binary/Module.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::runtime
{

/// The value of a constant instruction (i32/i64/f32/f64/v128.const) of @p module, or nullopt for any
/// other instruction.
/// @param module The module whose code holds @p instruction; a v128.const's bytes are among its literals.
/// @param instruction The instruction.
/// @return Its immediate's value, its bits unchanged, or nullopt.
[[nodiscard]] std::optional<Value> constantOf(binary::Module const& module,
                                              binary::Instruction const& instruction);

/// The null reference ref.null @p instruction of @p instance pushes, its heap in store form: a concrete
/// type is named by its TypeId (ModuleInstance::types).
/// @param instance The instance whose code holds @p instruction.
/// @param instruction A ref.null of @p instance's module.
/// @return The null of its heap.
[[nodiscard]] Value nullOf(ModuleInstance const& instance, binary::Instruction const& instruction);

/// The reference ref.func pushes for function @p index of @p instance.
/// @param instance The instance.
/// @param index The function's index in @p instance's function index space (checked).
/// @return A reference to the function's store address.
[[nodiscard]] Value functionReference(ModuleInstance const& instance, uint32_t index);

/// Evaluates a constant expression of a module instance, as instantiation does for every global
/// initializer, table initializer, element segment item and segment offset; no executor takes part.
///
/// It evaluates what validation admits in a constant expression: the constant instructions of every
/// value type (v128.const included), ref.null, ref.func and global.get, and extended-const's i32 and
/// i64 add, sub and mul, which wrap. None of these traps, so evaluation always yields a value.
/// @param store The store holding the instance.
/// @param moduleInstance The address of the instance whose globals and functions the expression may name.
/// @param expression The expression; valid for that instance's module (checked, as far as evaluation
///        reaches: an instruction outside the list above, an operand of another type, or a stack that
///        does not end with one value at end, is a contract violation).
/// @return Its value.
[[nodiscard]] Value evaluateConstantExpression(Store const& store,
                                               uint32_t moduleInstance,
                                               binary::ConstExpr const& expression);

} // namespace isoline::runtime
