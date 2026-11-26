// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include <isoline/binary/Module.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::interpreter
{

/// Executes Wasm by the spec's small-step semantics, for clarity. Never a production engine.
///
/// It is the oracle later engines are checked against, so it follows the spec's execution rules
/// (WebAssembly 3.0, chapter 4, for the proposals the decoder and validator accept) one instruction at a
/// time, over a validated module, and favours being obviously right over being fast. Operands, locals, labels
/// and call frames live in heap vectors: a Wasm call never recurses on the host stack, and a call deeper than
/// the store's StoreLimits::maxCallDepth frames traps with CallStackExhausted. So does a call whose frame's
/// static cost (validate::ValidatedModule::frameCost(), runtime::hostFrameCost()) would take the store's
/// charged sum (runtime::Store::chargeFrame()) past StoreLimits::maxStackValues: a cost from validation, the
/// same in every engine. A host function runs synchronously, on the host stack, and may invoke Wasm again
/// through its runtime::HostContext; each such nested invocation does recurse on the host stack, and
/// runtime::invokeChecked() lets at most StoreLimits::maxNestedInvocations be in progress at once. The
/// charged sum is the store's, so nested invocations share the stack cap rather than multiply it.
///
/// An instance is not thread-safe: its depth counter belongs to one thread at a time. Threads that
/// execute concurrently each use an interpreter of their own.
///
/// An exception unwinds the label and frame stacks in a loop, never recursing on the host stack, to
/// the innermost try_table that catches it. One no try_table of the invocation catches ends it with
/// Thrown; a host function that returns it to a Wasm caller throws it on there. Exceptions stay in
/// the store (Store::addException), within its StoreLimits.
///
/// Floats follow Numerics: where the spec allows a set of NaNs, the result is the canonical NaN.
/// Float results assume the default floating-point environment: round-to-nearest, with subnormals
/// neither flushed to zero nor treated as zero (no FTZ or DAZ). An embedder that changes it must
/// restore it before invoking; debug builds check the rounding mode on entry.
class ReferenceInterpreter final: public runtime::ICodeExecutor
{
  public:
    ReferenceInterpreter() = default;

    /// Invokes a function, as the spec's invocation does (4.5.5).
    ///
    /// Precondition (checked): an invocation is open on @p store (runtime::InvocationScope), as
    /// runtime::invokeChecked() and runtime::instantiate() open one; @p function exists, @p args are of
    /// its parameter types, reference
    /// heaps and addresses included (runtime::checkValues()), and @p results has room for exactly its
    /// results. runtime::invokeChecked() checks all three first and refuses a call that fails them,
    /// so every caller but instantiation's start call goes through it. What a host function it calls
    /// ends with is checked too (runtime::checkHostOutcome()): a result not of its type, or a thrown
    /// exception the store does not hold, ends the invocation with a HostFailure.
    /// @param store The store holding it.
    /// @param function Its address.
    /// @param args Its arguments.
    /// @param results Receives its results.
    /// @return Nothing, or how execution ended abruptly: the trap; Thrown, the exception no handler
    ///         caught; or the HostFailure of a host function it called.
    [[nodiscard]] std::expected<void, runtime::Abrupt> invoke(runtime::Store& store,
                                                              runtime::FuncAddr function,
                                                              std::span<runtime::Value const> args,
                                                              std::span<runtime::Value> results) override;

    /// Evaluates a constant expression: the constant instructions of every value type (v128.const
    /// included), ref.null, ref.func and global.get, and extended-const's i32 and i64 add, sub and mul.
    /// @param store The store holding the instance.
    /// @param moduleInstance The instance whose globals and functions the expression names.
    /// @param expression The expression; valid for that instance's module.
    /// @return Its value.
    [[nodiscard]] std::expected<runtime::Value, runtime::Trap> evaluateConst(
        runtime::Store& store, uint32_t moduleInstance, binary::ConstExpr const& expression) override;

  private:
    /// The Wasm frames live across every invocation in progress: a host function that invokes
    /// Wasm again nests within the depth of the call that reached it.
    uint32_t _depth = 0;
};

} // namespace isoline::interpreter
