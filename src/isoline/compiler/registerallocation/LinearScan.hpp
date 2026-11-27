// SPDX-License-Identifier: Apache-2.0
#pragma once

// Register allocation: linear scan on SSA with lifetime holes and interval splitting, after Wimmer and Franz,
// "Linear Scan Register Allocation on SSA Form" (2010).
//
// The allocator takes a selected function (critical edges split, MachineFunction), lays it out (linearize())
// and computes its live intervals (computeLiveIntervals()), then walks the intervals' children in order of
// their first live position. Each register keeps the ranges it holds in an ordered map, so a child may take a
// register through another's lifetime hole. A child takes a register free over all of it; else one free over
// a prefix that reaches its next use, split there; else it evicts the holders of the register they all next
// need latest, when that is after its own use, or waits in its stack slot up to its next use. An evicted
// remainder stays in its slot, with a window child around each use that is never evicted itself, so no range
// enters a register twice after an eviction; the number of children is bounded by the values and uses before
// the scan starts (a scan past the bound is a bug, refused), and each step costs O(log n).
//
// Positions are liveness's: even for block entries and instructions, odd for the gaps between, where every
// move goes (MachineFunction::movesBefore()). An instruction reads a use at p from the location its value has
// at p − 1 and writes a definition to the location at p. A value the instruction only reads may leave its
// register at that read: it is stored to its slot in the gap before, the instruction still reads the
// register, and a definition at p may take it. Fixed operands are pre-coloured: the fixed register is taken
// from the gap before to the instruction for a use, copied into there, and from the instruction to the gap
// after for a definition, copied out of there; physical register operands take their register from definition
// to last read. So a call, whose definitions are fixed to every register it clobbers, moves every value live
// across it to its stack slot. A tied operand (UseDef) reads and writes one location by construction. An
// Early definition starts in the gap before its instruction, where it overlaps the uses.
//
// The moves of one gap are parallel and are sequentialized (sequentializeParallelMoves()) through one scratch
// slot, in phases: the moves of an edge into a block with one predecessor; where children change place, and
// the copies out of fixed registers; the copies into fixed registers, and the stores of values leaving their
// register at a read; the moves of an edge out of a block with one successor, whose terminator reads no
// register. Spill slots are one per value, reused once a value's lifetime is over, from a min-heap keyed by
// when each slot becomes free (P5 Decision "Register allocation"). Every operand position needs a register:
// a memory operand is not taken in place of a reload (AllowsMemory is a later refinement).

#include <cstdint>
#include <expected>
#include <memory_resource>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/CompileLimits.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/Target.hpp>

namespace isoline::compiler::registerallocation
{

/// A register allocator: rewrites a selected function's virtual registers to physical ones in place, inserts
/// the moves between them, and lays out its frame.
class IRegisterAllocator
{
  public:
    virtual ~IRegisterAllocator() = default;

    /// Allocates @p function's registers on @p target.
    /// @param function A selected function, verified (verifyMachineFunction(), checked), its critical edges
    ///                 split. On success every register operand names a physical register and every
    ///                 instruction has its moves; on failure it is left half-rewritten, to be dropped.
    /// @param target The target it was selected for.
    /// @param frameCost The Wasm stack values the function charges: what its frame is bounded by.
    /// @param cancellation Polled every 4'096 intervals.
    /// @param memory What the temporaries allocate from.
    /// @return The frame; FrameTooLarge past the target's frame rule; LiveSetTooLarge or Cancelled; or
    ///         InternalVerificationFailed (pass "register-allocation", "liveness", "frame" or
    ///         "machine-verifier") for a function the allocator cannot take: a critical edge, an instruction
    ///         asking for more registers than there are, a compiler bug refused, never run.
    [[nodiscard]] virtual std::expected<machine::FrameLayout, CompileError> allocate(
        machine::MachineFunction& function,
        machine::ITarget const& target,
        uint64_t frameCost,
        CompileCancellation const& cancellation,
        std::pmr::memory_resource& memory) const = 0;

  protected:
    IRegisterAllocator() = default;
    IRegisterAllocator(IRegisterAllocator const&) = default;
    IRegisterAllocator(IRegisterAllocator&&) = default;
    IRegisterAllocator& operator=(IRegisterAllocator const&) = default;
    IRegisterAllocator& operator=(IRegisterAllocator&&) = default;
};

/// The linear-scan allocator. Stateless once constructed: it holds only the compile's caps, which its
/// liveness is bounded by.
class LinearScanAllocator final: public IRegisterAllocator
{
  public:
    /// An allocator under @p limits (maxLiveSetBits).
    explicit LinearScanAllocator(CompileLimits const& limits) noexcept: _limits { limits } {}

    [[nodiscard]] std::expected<machine::FrameLayout, CompileError> allocate(
        machine::MachineFunction& function,
        machine::ITarget const& target,
        uint64_t frameCost,
        CompileCancellation const& cancellation,
        std::pmr::memory_resource& memory) const override;

  private:
    CompileLimits _limits;
};

} // namespace isoline::compiler::registerallocation
