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
    ForeignHandle,     ///< The function is another store's (or a store's that is gone): no guest code ran.
    StoreDeleted,      ///< The instance's store is gone: nothing ran, and nothing was read.
};

/// The last CallFailure enumerator; it must name whichever enumerator is listed last above. The C API's
/// error rows (c/detail/ErrorRows.hpp) hold one row per enumerator up to it.
inline constexpr auto LastCallFailure = CallFailure::StoreDeleted;

/// A handle of another store than the one asked (isOf() is false): its entity is not this store's, so
/// nothing was read or changed. A handle of a store that is gone is foreign to every live store.
struct ForeignHandle
{
    /// All such refusals are equal.
    bool operator==(ForeignHandle const&) const = default;
};

/// A handle used after its store was destroyed: the store's alive-token has expired, so nothing was
/// read or changed.
struct StoreDeleted
{
    /// All such refusals are equal.
    bool operator==(StoreDeleted const&) const = default;
};

/// An exception the guest threw and nothing caught, copied out of the store.
///
/// It is a copy, so the store's slot stays reclaimable: the exception arena never fills with
/// exceptions the embedder has already been told about. Exnrefs within the payload are escaped
/// (runtime::Store::markEscaped()), so they stay valid for the store's life.
struct UncaughtException
{
    TagAddr tag = 0;            ///< Its tag, which compares with Instance::tag()'s Tag::address().
    std::vector<Value> payload; ///< The tag's params' values, in order.

    /// Equal when tag and payload are.
    bool operator==(UncaughtException const&) const = default;
};

/// A failed call: how it ended, and the growth this call was refused.
struct CallError
{
    /// How it ended: the trap; a host function's failure; the exception nobody caught; or, for a call
    /// that was refused before it entered the guest, the first argument (or result count) that did not
    /// fit, a function of another store, or an instance whose store is gone.
    using Detail =
        std::variant<Trap, HostFailure, UncaughtException, ValueMismatch, ForeignHandle, StoreDeleted>;

    /// How it ended: the one truth failure() reads.
    ///
    /// Trap{TrapCode::HostTrap, subject} is reserved for the embedder: a host function ends the call
    /// with it (WASI's proc_exit, say, with the exit code as subject), and every engine hands it back
    /// unchanged, subject included, so the embedder reads its own value back.
    Detail detail;

    /// This call's first refused memory.grow and table.grow, never a nested call's: why a guest that
    /// gave up after a -1 from memory.grow gave up. For a call made in an InvocationScope, and for its
    /// Store::poll(), the invocation's: every call made directly in the scope so far.
    GrowthRecord refusedGrowth;

    /// Why the call failed, derived from detail: a Trap of TrapCode::Interrupted is Interrupted, every
    /// other Trap is Trap, a ValueMismatch is ArgumentMismatch, and every other alternative is the
    /// CallFailure of its name.
    /// @return The failure.
    [[nodiscard]] CallFailure failure() const noexcept;

    /// Equal when detail and refusedGrowth are.
    bool operator==(CallError const&) const = default;
};

/// An initial value for a table's new elements (Store::growTable()) that is not of the table's element
/// type: nothing was grown.
struct InitValueMismatch
{
    /// All such refusals are equal.
    bool operator==(InitValueMismatch const&) const = default;
};

/// Why growing a memory failed, its size unchanged: a memory of another store, or the guest's own
/// refusal (Refusal), as memory.grow is refused, the store's limits charged alike.
using MemoryGrowthError = std::variant<ForeignHandle, Refusal>;

/// Why growing a table (Store::growTable()) failed, its size unchanged: a table of another store; an
/// initial value not of its element type; or the guest's own refusal (Refusal), as table.grow is
/// refused, the store's limits charged alike.
using TableGrowthError = std::variant<ForeignHandle, InitValueMismatch, Refusal>;

} // namespace isoline::api
