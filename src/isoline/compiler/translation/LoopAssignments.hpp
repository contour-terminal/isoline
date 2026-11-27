// SPDX-License-Identifier: Apache-2.0
#pragma once

// The locals each loop of a function body assigns, found in one pass over the decoded body before
// translation, without recursion and within a budget: what each loop header takes as parameters.

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/compiler/CompileError.hpp>

namespace isoline::compiler::translation
{

/// The locals assigned inside each loop of a function body, an inner loop's counted in every loop around
/// it. Loops are numbered by ordinal: in body order of their loop instruction, from 0. Each set is sorted
/// by local index, parameters included in the index space.
class LoopAssignments
{
  public:
    /// Where one loop's set lies in the pool of every set.
    struct Run
    {
        uint32_t begin = 0; ///< Its first entry.
        uint32_t count = 0; ///< How many.
    };

    /// No loops, every container allocating from @p memory.
    /// @param memory What the sets allocate from.
    explicit LoopAssignments(std::pmr::memory_resource& memory);

    /// How many loops the body has.
    [[nodiscard]] uint32_t loopCount() const noexcept { return static_cast<uint32_t>(_bodyIndices.size()); }

    /// The locals assigned inside loop @p ordinal, inner loops included, in ascending order.
    /// @param ordinal A loop's ordinal, below loopCount() (Require).
    [[nodiscard]] std::span<uint32_t const> locals(uint32_t ordinal) const noexcept;

    /// The body index of loop @p ordinal's loop instruction.
    /// @param ordinal A loop's ordinal, below loopCount() (Require).
    [[nodiscard]] uint32_t bodyIndex(uint32_t ordinal) const noexcept;

    /// The ordinal of the first loop at or after @p bodyIndex: where a translation that skips
    /// dead code up to @p bodyIndex resumes counting.
    /// @return It; loopCount() when none is.
    [[nodiscard]] uint32_t firstAtOrAfter(uint32_t bodyIndex) const noexcept;

  private:
    friend std::expected<LoopAssignments, CompileError> assignedLocalsPerLoop(
        std::span<binary::Instruction const> body,
        uint64_t maxTotalBlockParameters,
        std::pmr::memory_resource& memory);

    std::pmr::vector<uint32_t> _bodyIndices; ///< Per loop, its loop instruction.
    std::pmr::vector<Run> _runs;             ///< Per loop, its set.
    std::pmr::vector<uint32_t> _locals;      ///< Every set, loop after loop as they close.
};

/// The locals each loop of @p body assigns (local.set, local.tee), an inner loop's merged into the loop
/// around it at its end. Iterative and linear: each local joins each set once, and every
/// joining, direct or merged, is counted against @p maxTotalBlockParameters, so 100'000 nested loops around
/// 50'000 distinct assignments cost at most the cap, never their product. No set is keyed by a hash.
/// @param body A validated function body, ending with its final end.
/// @param maxTotalBlockParameters CompileLimits::maxTotalBlockParameters: the most joinings, over all sets.
/// @param memory What the result and the temporaries allocate from.
/// @return The sets, by loop ordinal; or TooManyBlockParameters (without a function: the caller
///         names it), requested the joinings at the refusal, limit the cap.
[[nodiscard]] std::expected<LoopAssignments, CompileError> assignedLocalsPerLoop(
    std::span<binary::Instruction const> body,
    uint64_t maxTotalBlockParameters,
    std::pmr::memory_resource& memory);

} // namespace isoline::compiler::translation
