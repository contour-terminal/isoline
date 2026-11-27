// SPDX-License-Identifier: Apache-2.0
#pragma once

// The optimization passes, as data: one row per run of a pass, in the order the compiler runs them. Each
// pass rewrites a well-formed function into a well-formed one (intermediate::verify()), in time linear in
// the function (O(n log n) at most), allocating every temporary from the compile's arena, so that its cost
// is charged to the compile's budget like translation's. Adding a pass is adding a row.
//
// What every pass keeps, by construction: an instruction whose effect is not Pure or ReadsContext is never
// removed while a path reaches it, nor moved, nor merged with another; so every reachable poll, pollStored,
// enterFrame, leaveFrame, storeContext, call, store and trap stays where translation put it, and every
// guard (checkMemory, checkTable, checkDispatch) with it. No pass folds an offset into an address, merges two
// guards, or reuses one guard's result for another access: guardAddress is only ever created by
// EliminateBoundsChecks (P5 Task 8), and no other pass creates or rewrites a Pointer. Code no path reaches
// (after a trap, or in a block no edge enters) goes with its polls and guards, which never run.

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::compiler::optimization
{

/// An optimization pass.
enum class Pass : uint8_t
{
    RemoveTrivialParameters, ///< Replaces a parameter every edge passes the same value by that value.
    FoldConstants,           ///< Evaluates operations on constants, and branches and traps on them.
    SimplifyControlFlow,     ///< Removes unreachable blocks, threads forwarding blocks, merges chains.
    EliminateBoundsChecks,   ///< Removes the memory checks a constant or a dominating check proves.
    EliminateDeadCode,       ///< Removes unused Pure and ReadsContext instructions and unused parameters.
};

/// The last Pass enumerator.
inline constexpr auto LastPass = Pass::EliminateDeadCode;

/// Whether the pass manager verifies the function after each pass.
///
/// A pass that breaks the function is a compiler bug, so a failed verification is a precondition violation
/// (Require), never a CompileError. Tests and debug builds pass Yes: every case that compiles runs the
/// verifier after every pass, the differential fuzzer's included. A release build passes No: the verifier
/// builds a dominator tree and walks every use, a cost of the same order as the passes themselves, six times
/// over, and it checks the compiler rather than the guest, which validation already checked. (P5 Task 15's
/// Compiler picks it by its build.)
enum class VerifyAfterEachPass : uint8_t
{
    No,  ///< Trust the passes.
    Yes, ///< Verify after each pass.
};

/// What every pass reads: the module, the configuration, the cancellation, the arena.
struct PassContext
{
    validate::ValidatedModule const& module; ///< The module the function is of.
    CompileConfig const& config;             ///< How it is compiled.
    CompileCancellation const& cancellation; ///< Polled every CancellationInterval units of work.
    std::pmr::memory_resource& memory;       ///< The compile's arena: every temporary allocates from it.
    VerifyAfterEachPass verify = VerifyAfterEachPass::Yes; ///< Whether runPasses() verifies after each pass.
    uint32_t codeIndex = 0; ///< The function's index in the code section, for errors.
};

/// What a pass ends with: nothing, or Cancelled. A cancelled pass may leave the function half rewritten;
/// the compile then discards it.
using PassOutcome = std::expected<void, CompileError>;

/// A pass's entry point.
using PassFunction = PassOutcome (*)(intermediate::Function&, PassContext&);

/// One run of a pass.
struct PassRow
{
    Pass pass;             ///< The pass.
    std::string_view name; ///< Its name in diagnostics.
    PassFunction run;      ///< Runs it.
};

/// Replaces each block parameter that every edge passes one value (itself aside) by that value, and drops
/// it with its arguments: the strongly connected groups of parameters that pass only each other and one
/// outside value collapse to it, found by one explicit-stack Tarjan walk in O(arguments).
PassOutcome removeTrivialParameters(intermediate::Function& function, PassContext& context);

/// Replaces each operation of FoldingTable whose operands are all constants by the constant its kernel
/// computes, through numerics:: as both interpreters do, and a kernel's trap by a trap terminator ending the
/// block; removes a trapIf or trapIfSubject on zero, turns a trapIf on another constant into a trap
/// terminator, and a branch or switch on a constant into a jump to the target it takes. Blocks that no
/// longer reach are removed. Float arithmetic runs in the host's floating-point environment, which the
/// compile pins (runtime::FloatEnvironmentScope, P5 Task 15).
PassOutcome foldConstants(intermediate::Function& function, PassContext& context);

/// Removes the blocks no path reaches; redirects every edge into a forwarding block (one holding only a jump,
/// passing no more values than it takes, whose parameters nothing else uses) to where the forwarding chain
/// ends, never through a block holding anything else, a poll included; then merges each block that only
/// jumps into a block with no other predecessor with that block.
PassOutcome simplifyControlFlow(intermediate::Function& function, PassContext& context);

/// Removes the memory checks a constant or a dominating check proves redundant. P5 Task 8 implements it;
/// until then it is the identity, so that the pass order is the one the plan's Decisions fix.
PassOutcome eliminateBoundsChecks(intermediate::Function& function, PassContext& context);

/// Removes every Pure and ReadsContext instruction no kept instruction, return or branch condition needs,
/// and every parameter (the entry's aside) no such use needs, with the arguments passed to it: liveness is
/// marked from the effects and terminators by one worklist, so a value used only to compute itself around
/// a loop goes too.
PassOutcome eliminateDeadCode(intermediate::Function& function, PassContext& context);

/// Every pass in the order the compiler runs them (the plan's Decisions): trivial parameters first, so that
/// folding sees through them; folding before control-flow simplification, which removes what folding
/// disconnected; dead code after bounds checks, which leave dead index computations; and control flow
/// again, to merge what dead code emptied.
inline constexpr auto PassTable = std::to_array<PassRow>({
    { .pass = Pass::RemoveTrivialParameters,
      .name = "remove-trivial-parameters",
      .run = removeTrivialParameters },
    { .pass = Pass::FoldConstants, .name = "fold-constants", .run = foldConstants },
    { .pass = Pass::SimplifyControlFlow, .name = "simplify-control-flow", .run = simplifyControlFlow },
    { .pass = Pass::EliminateBoundsChecks, .name = "eliminate-bounds-checks", .run = eliminateBoundsChecks },
    { .pass = Pass::EliminateDeadCode, .name = "eliminate-dead-code", .run = eliminateDeadCode },
    { .pass = Pass::SimplifyControlFlow, .name = "simplify-control-flow", .run = simplifyControlFlow },
});

static_assert(
    [] {
        for (auto const pass: std::views::iota(0U, std::to_underlying(LastPass) + 1U))
            if (std::ranges::none_of(PassTable, [pass](PassRow const& row) {
                    return std::to_underlying(row.pass) == pass && row.run != nullptr && !row.name.empty();
                }))
                return false;
        return true;
    }(),
    "every Pass must have a named row in PassTable");

/// Runs @p passes over @p function in their order, verifying after each when the context says so (a failed
/// verification is a precondition violation, the pass and the fault written to stderr first), then
/// compacts the function (Function::compact()).
/// @param function A well-formed function (intermediate::verify()).
/// @param context What the passes read.
/// @param passes The rows to run; PassTable for a compile.
/// @return Nothing; or Cancelled, the function then half rewritten.
[[nodiscard]] PassOutcome runPasses(intermediate::Function& function,
                                    PassContext& context,
                                    std::span<PassRow const> passes);

/// Runs PassTable over @p function (runPasses(function, context, PassTable)).
[[nodiscard]] PassOutcome runPasses(intermediate::Function& function, PassContext& context);

} // namespace isoline::compiler::optimization
