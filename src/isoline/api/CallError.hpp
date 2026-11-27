// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include <isoline/api/Types.hpp>

namespace isoline::api
{

/// Why a call into a guest failed, in the fewest words an embedder branches on. CallError::failure()
/// derives it from the error's detail.
enum class CallFailure : uint8_t
{
    Trap,              ///< The guest, or a host function ending the call as one, trapped.
    Interrupted,       ///< The store's interrupter stopped the call, or had before it began.
    HostFailure,       ///< A host function the guest called failed, and the failure reached the caller.
    UncaughtException, ///< The guest threw an exception no handler caught.
    ArgumentMismatch,  ///< The arguments, or the number of result slots, do not fit the function: no
                       ///< guest code ran.
};

/// An exception the guest threw and nothing caught, copied out of the store.
///
/// It is a copy, so the store's slot stays reclaimable: the exception arena never fills with
/// exceptions the embedder has already been told about. Exnrefs within the payload are escaped
/// (runtime::Store::markEscaped()), so they stay valid for the store's life.
struct UncaughtException
{
    TagAddr tag = 0;            ///< Its tag, which compares with Instance::tag().
    std::vector<Value> payload; ///< The tag's params' values, in order.

    /// Equal when tag and payload are.
    bool operator==(UncaughtException const&) const = default;
};

/// A failed call: how it ended, and the growth this call was refused.
struct CallError
{
    /// How it ended: the trap; a host function's failure; the exception nobody caught; or, for a call
    /// that was refused before it entered the guest, the first argument (or result count) that did not
    /// fit.
    using Detail = std::variant<Trap, HostFailure, UncaughtException, ValueMismatch>;

    /// How it ended: the one truth failure() reads.
    ///
    /// Trap{TrapCode::HostTrap, subject} is reserved for the embedder: a host function ends the call
    /// with it (WASI's proc_exit, say, with the exit code as subject), and every engine hands it back
    /// unchanged, subject included, so the embedder reads its own value back.
    Detail detail;

    /// This call's first refused memory.grow and table.grow, never a nested call's: why a guest that
    /// gave up after a -1 from memory.grow gave up.
    GrowthRecord refusedGrowth;

    /// Why the call failed, derived from detail: a Trap of TrapCode::Interrupted is Interrupted, every
    /// other Trap is Trap, and a ValueMismatch is ArgumentMismatch.
    /// @return The failure.
    [[nodiscard]] CallFailure failure() const noexcept;

    /// Equal when detail and refusedGrowth are.
    bool operator==(CallError const&) const = default;
};

} // namespace isoline::api
