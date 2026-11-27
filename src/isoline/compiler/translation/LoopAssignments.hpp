// SPDX-License-Identifier: Apache-2.0
#pragma once

// The locals each block construct of a function body assigns, found in one pass over the decoded body
// before translation, without recursion and within a budget. A loop's set is what its header takes as
// parameters; a block's or an if's set is what its forward merges may need parameters for, every other
// local reaching their end with the value it had on entry.

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/compiler/CompileError.hpp>

namespace isoline::compiler::translation
{

/// The locals assigned inside each block, loop and if of a function body, a nested construct's counted
/// in every construct around it. Constructs are numbered by ordinal: in body order of their opening
/// instruction, from 0. Each set is sorted by local index, parameters included in the index space.
class LoopAssignments
{
  public:
    /// Where one construct's set lies in the pool of every set.
    struct Run
    {
        uint32_t begin = 0; ///< Its first entry.
        uint32_t count = 0; ///< How many.
    };

    /// No constructs, every container allocating from @p memory.
    /// @param memory What the sets allocate from.
    explicit LoopAssignments(std::pmr::memory_resource& memory);

    /// How many constructs the body opens.
    [[nodiscard]] uint32_t constructCount() const noexcept
    {
        return static_cast<uint32_t>(_bodyIndices.size());
    }

    /// The locals assigned inside construct @p ordinal, nested constructs included, in ascending order.
    /// @param ordinal A construct's ordinal, below constructCount() (Require).
    [[nodiscard]] std::span<uint32_t const> locals(uint32_t ordinal) const noexcept;

    /// The body index of the instruction that opens construct @p ordinal.
    /// @param ordinal A construct's ordinal, below constructCount() (Require).
    [[nodiscard]] uint32_t bodyIndex(uint32_t ordinal) const noexcept;

    /// The ordinal of the first construct opened at or after @p bodyIndex: where a translation that skips
    /// dead code up to @p bodyIndex resumes counting.
    /// @return It; constructCount() when none is.
    [[nodiscard]] uint32_t firstAtOrAfter(uint32_t bodyIndex) const noexcept;

  private:
    friend std::expected<LoopAssignments, CompileError> assignedLocalsPerLoop(
        std::span<binary::Instruction const> body,
        uint64_t maxTotalBlockParameters,
        std::pmr::memory_resource& memory);

    std::pmr::vector<uint32_t> _bodyIndices; ///< Per construct, its opening instruction.
    std::pmr::vector<Run> _runs;             ///< Per construct, its set.
    std::pmr::vector<uint32_t> _locals;      ///< Every set, construct after construct as they close.
};

/// The locals each block, loop and if of @p body assigns (local.set, local.tee), a nested construct's
/// merged into its parent's at its end. Iterative and linear: each local joins each set once, and every
/// joining, direct or merged, is counted against @p maxTotalBlockParameters, so 100'000 nested loops around
/// 50'000 distinct assignments cost at most the cap, never their product. No set is keyed by a hash.
/// @param body A validated function body, ending with its final end.
/// @param maxTotalBlockParameters CompileLimits::maxTotalBlockParameters: the most joinings, over all sets.
/// @param memory What the result and the temporaries allocate from.
/// @return The sets, by construct ordinal; or TooManyBlockParameters (without a function: the caller
///         names it), requested the joinings at the refusal, limit the cap.
[[nodiscard]] std::expected<LoopAssignments, CompileError> assignedLocalsPerLoop(
    std::span<binary::Instruction const> body,
    uint64_t maxTotalBlockParameters,
    std::pmr::memory_resource& memory);

} // namespace isoline::compiler::translation
