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
// guards, or reuses one guard's result for another access, but EliminateBoundsChecks: it alone removes
// checkMemory, by intermediate/Guards.hpp's two rules (a constant index within the memory's declared minimum
// becomes inBoundsAddress; a check a dominating one covers takes its address, through guardAddress), counting
// each in PassStatistics, and no other pass creates or rewrites a Pointer. Code no path reaches (after a
// trap, or in a block no edge enters) goes with its polls and guards, which never run.

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

/// When the pass manager verifies the function (intermediate::verify()).
///
/// A pass that breaks the function is a compiler bug. Verification turns it into a refusal of the function,
/// CompileErrorCode::InternalVerificationFailed naming the pass and the rule, so that miscompiled code never
/// runs; it never aborts the process. A release build verifies once, at the end: one dominator tree and one
/// walk of every use, against the passes' six. Tests and debug builds verify after every pass, so the error
/// names the pass that broke the rule. (P5 Task 15's Compiler picks it by its build.)
enum class Verification : uint8_t
{
    AtEnd,         ///< Once, after the last pass: a release compile.
    AfterEachPass, ///< After every pass: tests and debug builds.
};

/// What a pass counts: the rows of PassStatisticTable.
enum class PassStatistic : uint8_t
{
    /// checkMemory of a constant index within its memory's declared minimum, become inBoundsAddress.
    ChecksProvedByMinimum,
    /// checkMemory a dominating check covered at the same offset, its uses now the dominating check's
    /// address.
    ChecksSharingDominatingAddress,
    /// checkMemory a dominating check covered further on, become guardAddress of the dominating address.
    ChecksMovedFromDominatingAddress,
};

/// The last PassStatistic enumerator.
inline constexpr auto LastPassStatistic = PassStatistic::ChecksMovedFromDominatingAddress;

/// How many PassStatistic enumerators there are.
inline constexpr auto PassStatisticCount = std::size_t { std::to_underlying(LastPassStatistic) } + 1;

/// One PassStatistic: its name in diagnostics.
struct PassStatisticRow
{
    PassStatistic statistic; ///< The statistic; equal to the row's index.
    std::string_view name;   ///< Its name.
};

/// Every PassStatistic's row, in enumerator order.
inline constexpr auto PassStatisticTable = std::to_array<PassStatisticRow>({
    { .statistic = PassStatistic::ChecksProvedByMinimum, .name = "checks-proved-by-minimum" },
    { .statistic = PassStatistic::ChecksSharingDominatingAddress,
      .name = "checks-sharing-dominating-address" },
    { .statistic = PassStatistic::ChecksMovedFromDominatingAddress,
      .name = "checks-moved-from-dominating-address" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: PassStatisticTable)
            if (std::to_underlying(row.statistic) != index++)
                return false;
        return index == PassStatisticCount;
    }(),
    "PassStatisticTable must hold one row per PassStatistic, at the index of its enumerator");

/// What the passes of one run counted, by PassStatistic: every check a pass eliminated shows here.
class PassStatistics
{
  public:
    /// How many @p statistic counted.
    [[nodiscard]] uint64_t count(PassStatistic statistic) const noexcept
    {
        return _counts[std::to_underlying(statistic)];
    }

    /// Counts @p amount more of @p statistic.
    void add(PassStatistic statistic, uint64_t amount = 1) noexcept
    {
        _counts[std::to_underlying(statistic)] += amount;
    }

  private:
    std::array<uint64_t, PassStatisticCount> _counts {};
};

/// What every pass reads: the module, the configuration, the cancellation, the arena; and what they count.
struct PassContext
{
    validate::ValidatedModule const& module; ///< The module the function is of.
    CompileConfig const& config;             ///< How it is compiled.
    CompileCancellation const& cancellation; ///< Polled every CancellationInterval units of work.
    std::pmr::memory_resource& memory;       ///< The compile's arena: every temporary allocates from it.
    Verification verification = Verification::AtEnd; ///< When runPasses() verifies.
    uint32_t codeIndex = 0; ///< The function's index in the code section, for errors.
    /// Per memory of the module's index space, imports first, its declared minimum in bytes
    /// (memoryMinimumBytesOf()): what EliminateBoundsChecks proves a constant index against and the verifier
    /// holds inBoundsAddress to. Empty proves nothing: every such check stays.
    std::span<uint64_t const> memoryMinimumBytes {};
    PassStatistics statistics {}; ///< What the passes counted; runPasses() adds to it.
};

/// Every memory's declared minimum in bytes (intermediate::minimumBytesOf()), imports first, for
/// PassContext::memoryMinimumBytes: worked out once per module. An imported memory's is its import's
/// minimum, which import matching guarantees the memory bound to it reaches.
/// @param module The module.
/// @param memory What the result allocates from.
[[nodiscard]] std::pmr::vector<uint64_t> memoryMinimumBytesOf(validate::ValidatedModule const& module,
                                                              std::pmr::memory_resource& memory);

/// What a pass ends with: nothing, or Cancelled; what runPasses() ends with may also be
/// InternalVerificationFailed. A failed run may leave the function half rewritten; the compile then discards
/// it.
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

/// Removes the memory checks a constant or a dominating check proves redundant, by intermediate/Guards.hpp's
/// rules: a constant index whose access ends within the memory's declared minimum becomes inBoundsAddress;
/// a check whose bytes a dominating check of the same memory and SSA index covered, with nothing that may
/// grow or move a memory (MemoryMotionTable) on any path between, takes that check's address, through
/// guardAddress when it lies further on. One explicit-stack walk of the dominator tree, keeping per index the
/// widest check in scope, after a linear pass that numbers the stretches free of memory motion. The rewrite
/// is the same in both SpectreGuard modes: the clamp is the dominating check's select, which guardAddress
/// keeps; the mode decides only guardAddress's lowering. Counts each check in PassStatistics.
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

/// Runs @p passes over @p function in their order, verifying when the context says (Verification), then
/// compacts the function (Function::compact()).
/// @param function A well-formed function (intermediate::verify()).
/// @param context What the passes read.
/// @param passes The rows to run; PassTable for a compile.
/// @return Nothing; or Cancelled; or InternalVerificationFailed naming the pass ("end" when verified only at
///         the end) and the rule, the function then ill-formed and not compacted.
[[nodiscard]] PassOutcome runPasses(intermediate::Function& function,
                                    PassContext& context,
                                    std::span<PassRow const> passes);

/// Runs PassTable over @p function (runPasses(function, context, PassTable)).
[[nodiscard]] PassOutcome runPasses(intermediate::Function& function, PassContext& context);

} // namespace isoline::compiler::optimization
