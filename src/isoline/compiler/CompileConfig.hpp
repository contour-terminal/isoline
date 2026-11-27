// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

#include <isoline/compiler/CompileLimits.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/runtime/ExecutionMode.hpp>

namespace isoline::compiler
{

/// Whether generated code clamps a checked address under misspeculation (spec §10): a memory access's
/// effective pointer becomes 0 once its bounds check fails, a table index its bound, a br_table index
/// its default entry, each with a conditional move, so that a mispredicted check reads nothing it
/// must not. Enabled by default, as Wasmtime's dynamic heaps are.
enum class SpectreGuard : uint8_t
{
    Disabled, ///< Bounds checks branch only.
    Enabled,  ///< Every checked address is also clamped.
};

/// Whether a compile was cancelled (CompileCancellation).
enum class CancellationState : uint8_t
{
    Running,   ///< Not cancelled: the compile goes on.
    Cancelled, ///< Cancelled: the compile stops at its next poll and fails Cancelled.
};

/// A compile's cancellation flag: set from any thread, polled by the compiler between functions,
/// every 65'536 IR instructions in translation and passes, and every 4'096 intervals in register
/// allocation. Wait-free; a compile that sees it set fails CompileErrorCode::Cancelled.
class CompileCancellation
{
  public:
    /// Cancels the compile: every later poll sees Cancelled. Idempotent.
    void cancel() noexcept
    {
        _state.store(static_cast<uint8_t>(CancellationState::Cancelled), std::memory_order_release);
    }

    /// Whether the compile was cancelled.
    /// @return Cancelled once cancel() ran; Running before.
    [[nodiscard]] CancellationState state() const noexcept
    {
        return static_cast<CancellationState>(_state.load(std::memory_order_acquire));
    }

  private:
    std::atomic<uint8_t> _state { static_cast<uint8_t>(CancellationState::Running) };
};

/// How a module is compiled: for which target, in which execution mode, with which guard and within
/// which caps. Fixed at the Compiler's construction; equal configurations give byte-identical code,
/// and every field is part of an artifact's key (P8). The target and the mode have no default: every
/// caller names both, so no compile silently targets a default host or picks a mode by omission.
struct CompileConfig
{
    /// A configuration for @p target in @p mode.
    /// @param target What code is generated for.
    /// @param mode Production or Deterministic.
    /// @param spectre Whether checked addresses are also clamped; Enabled unless the caller opts out.
    /// @param limits The caps on the module's input; the published defaults unless the caller narrows them.
    CompileConfig(machine::TargetDescription target,
                  runtime::ExecutionMode mode,
                  SpectreGuard spectre = SpectreGuard::Enabled,
                  CompileLimits limits = {}) noexcept:
        target { target }, mode { mode }, spectre { spectre }, limits { limits }
    {
    }

    machine::TargetDescription target; ///< What code is generated for.
    /// Production or Deterministic: whether NaN results are canonicalized after every operation that
    /// may produce one.
    runtime::ExecutionMode mode;
    SpectreGuard spectre; ///< Whether checked addresses are also clamped.
    CompileLimits limits; ///< The caps on the module's input.

    /// Whether both configure the same compile.
    [[nodiscard]] bool operator==(CompileConfig const&) const = default;
};

} // namespace isoline::compiler
