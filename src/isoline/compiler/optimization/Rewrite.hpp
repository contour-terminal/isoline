// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the passes share to rewrite a function in linear time: the cancellation poll, a substitution of
// values resolved through its chains, the removal of block parameters with the arguments every edge passes
// them, the removal of unreachable blocks, and the per-value maps a pass reads (where a value is defined,
// which parameter it is, the edges into a block). Each walks the function once, with explicit worklists.

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>

namespace isoline::compiler::optimization
{

/// How much work a pass does between two looks at the compile's cancellation.
inline constexpr auto CancellationInterval = uint64_t { 65'536 };

/// Looks at the compile's cancellation every CancellationInterval units of a pass's work.
class CancellationPoll
{
  public:
    /// A poll of @p context's cancellation, naming its function when it fails.
    explicit CancellationPoll(PassContext const& context) noexcept;

    /// Counts @p units of work done.
    /// @return Nothing; or Cancelled, once an interval is complete and the compile was cancelled.
    [[nodiscard]] PassOutcome step(uint64_t units) noexcept;

  private:
    PassContext const* _context;
    uint64_t _sinceLastLook = 0;
};

/// Values to replace by others, each at most once; a replacement may itself be replaced, so a use resolves
/// through the chain. Path compression keeps every chain O(log n) amortized.
class Substitution
{
  public:
    /// No replacement yet, for a function of @p valueCount values.
    Substitution(uint32_t valueCount, std::pmr::memory_resource& memory);

    /// Makes every use of @p from a use of @p to. Preconditions (checked): @p from is replaced by nothing
    /// yet, and @p to does not resolve to @p from, which would close a cycle.
    void replace(intermediate::ValueId from, intermediate::ValueId to);

    /// What a use of @p value is a use of.
    [[nodiscard]] intermediate::ValueId resolve(intermediate::ValueId value) noexcept;

    /// Whether any value is replaced.
    [[nodiscard]] bool isEmpty() const noexcept { return _replaced == 0; }

    /// Rewrites every operand of every instruction a block lists, removed ones excepted, to what it resolves
    /// to.
    void applyTo(intermediate::Function& function);

  private:
    std::pmr::vector<intermediate::ValueId> _next; ///< Per value, what it is replaced by; itself if nothing.
    uint32_t _replaced = 0;                        ///< How many values are replaced.
};

/// What becomes of a block parameter.
enum class ParameterFate : uint8_t
{
    Kept,    ///< It stays.
    Dropped, ///< It goes, with the argument every edge into its block passes it.
};

/// Drops every parameter whose value @p fates marks Dropped, with its arguments: every terminator's runs
/// shrink in place, and so does every block's parameter list, in their order.
/// @param function The function; its uses of dropped parameters must already be gone.
/// @param fates One entry per value of the function; Dropped only on parameters of blocks other than the
///              entry, whose parameters are the function's (Require).
void dropParameters(intermediate::Function& function, std::span<ParameterFate const> fates);

/// Removes the blocks no path from the entry reaches (Function::removeBlocks()).
/// @return How many it removed.
uint32_t removeUnreachableBlocks(intermediate::Function& function, std::pmr::memory_resource& memory);

/// The blocks the entry reaches, in reverse postorder of a depth-first walk over their terminators'
/// targets in order: every block after the blocks that dominate it.
[[nodiscard]] std::pmr::vector<intermediate::BlockId> reversePostorderOf(
    intermediate::Function const& function, std::pmr::memory_resource& memory);

/// Per value, the instruction a block lists that defines it; Unnumbered for a parameter, and for a value
/// no listed instruction defines.
[[nodiscard]] std::pmr::vector<uint32_t> definitionsOf(intermediate::Function const& function,
                                                       std::pmr::memory_resource& memory);

/// Where each block parameter sits: its block and its position among the block's parameters.
struct ParameterPlaces
{
    std::pmr::vector<uint32_t> block;    ///< Per value, its block; Unnumbered when it is no parameter.
    std::pmr::vector<uint32_t> position; ///< Per value, its position; meaningful where block is.
};

/// The places of every parameter of every block.
[[nodiscard]] ParameterPlaces parameterPlacesOf(intermediate::Function const& function,
                                                std::pmr::memory_resource& memory);

/// One edge into a block: the pool index of the first argument it passes.
struct IncomingEdge
{
    uint32_t arguments = 0; ///< The operand pool index of its first argument; argument i is i further on.
};

/// Every edge into every block, block after block: the edges into block b are
/// edges[begins[b], begins[b + 1]).
struct IncomingEdges
{
    std::pmr::vector<uint32_t> begins;    ///< One entry per block and one past the last.
    std::pmr::vector<IncomingEdge> edges; ///< The edges, grouped by their target.

    /// The edges into @p block.
    [[nodiscard]] std::span<IncomingEdge const> into(intermediate::BlockId block) const noexcept
    {
        auto const index = std::to_underlying(block);
        return std::span { edges }.subspan(begins[index], begins[index + 1] - begins[index]);
    }
};

/// The edges into every block, from the terminators of every block that has one.
[[nodiscard]] IncomingEdges incomingEdgesOf(intermediate::Function const& function,
                                            std::pmr::memory_resource& memory);

/// The constant @p value is, when a listed constant instruction defines it.
/// @param function The function.
/// @param definitions Its definitionsOf().
/// @return The constant's bits: an integer's, a float's pattern; none when it is not a constant.
[[nodiscard]] std::optional<uint64_t> constantOf(intermediate::Function const& function,
                                                 std::span<uint32_t const> definitions,
                                                 intermediate::ValueId value) noexcept;

} // namespace isoline::compiler::optimization
