// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

#include <isoline/api/Types.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/runtime/Resources.hpp>

namespace isoline::component::runtime
{

/// What a canonical built-in reaches of its store while a core instance calls it. Task 15's
/// ComponentStore implements it; tests use a fake.
class IBuiltinHost
{
  public:
    virtual ~IBuiltinHost() = default;

    /// The store's resource state.
    /// @return It.
    [[nodiscard]] virtual ResourceStore& resources() noexcept = 0;

    /// Where a built-in's trap waits while it crosses the core frames as carrierTrap().
    /// @return The store's carrier.
    [[nodiscard]] virtual canonical::PendingTrap& pendingTrap() noexcept = 0;

    /// Runs a guest resource type's destructor on @p rep, as `resource.drop` does: the destructor lifted
    /// into its implementing instance and lowered into @p callerInstance (definitions.py's
    /// canon_resource_drop), so the call may re-enter the component, nested in the drop.
    /// @param implementor The resource type's implementor; it has a destructor.
    /// @param callerInstance The instance that dropped the handle.
    /// @param rep The resource's representation.
    /// @return Nothing, or the trap the call ended in.
    [[nodiscard]] virtual std::expected<void, Trap> callGuestDestructor(GuestImplementor const& implementor,
                                                                        uint32_t callerInstance,
                                                                        uint32_t rep) = 0;
};

/// `canon resource.new` (definitions.py's canon_resource_new): adds an own handle of @p rep to the
/// instance's table.
/// @param host The store.
/// @param instance The instance whose core code called it, which defines @p type.
/// @param type The runtime resource type of the built-in's type index there.
/// @param rep The representation.
/// @return The new handle's index; or CannotLeave, or Handles.
/// @throws std::bad_alloc When the host cannot allocate.
[[nodiscard]] std::expected<uint32_t, Trap> resourceNew(IBuiltinHost& host,
                                                        uint32_t instance,
                                                        ResourceTypeIndex type,
                                                        uint32_t rep);

/// `canon resource.drop` (definitions.py's canon_resource_drop): removes the handle at @p index; for an
/// own handle, runs the type's destructor (a host's IHostResourceDestructor::destroy(), or the guest's
/// through IBuiltinHost::callGuestDestructor()) after the handle is gone, so a destructor re-entering
/// the instance finds the slot free; for a borrow, ends its lend to the task it was lent to.
/// @param host The store.
/// @param instance The instance whose core code called it.
/// @param type The runtime resource type of the built-in's type index there.
/// @param index The guest's handle index.
/// @return Nothing; or CannotLeave, UnknownHandle, WrongHandleType, LendOutstanding, BorrowOutstanding
///         for a borrow whose call has ended, or the destructor's trap.
/// @throws std::bad_alloc When the host cannot allocate.
[[nodiscard]] std::expected<void, Trap> resourceDrop(IBuiltinHost& host,
                                                     uint32_t instance,
                                                     ResourceTypeIndex type,
                                                     uint32_t index);

/// `canon resource.rep` (definitions.py's canon_resource_rep): the representation of the handle at
/// @p index.
/// @param host The store.
/// @param instance The instance whose core code called it.
/// @param type The runtime resource type of the built-in's type index there.
/// @param index The guest's handle index.
/// @return The representation; or UnknownHandle or WrongHandleType.
/// @throws std::bad_alloc When a trap's message cannot be stored.
[[nodiscard]] std::expected<uint32_t, Trap> resourceRep(IBuiltinHost& host,
                                                        uint32_t instance,
                                                        ResourceTypeIndex type,
                                                        uint32_t index);

/// Whether P11 runs a canonical built-in (R6: the refusal is zero).
enum class BuiltinSupport : uint8_t
{
    Unimplemented, ///< Validation refuses it (🔀, 🧵, 📝 and ➡️ rows): P12 implements it.
    Implemented,   ///< Instantiation makes a core function of it.
};

/// What a built-in's core function is made for: its store, the instance that defines it, and the
/// runtime resource type its type index names there.
struct BuiltinBinding
{
    IBuiltinHost& host;        ///< The store; outlives the function.
    uint32_t instance = 0;     ///< The component instance whose `canon` defines it.
    ResourceTypeIndex type {}; ///< The runtime resource type of its type index.
};

/// A built-in's core function: its core type and implementation, for api::Store::define().
struct BuiltinFunction
{
    isoline::api::FuncType type;                           ///< Its core function type.
    std::shared_ptr<isoline::api::IHostFunction> function; ///< Its implementation; never null.
};

/// Makes a built-in's core function. A trap it raises is stored in IBuiltinHost::pendingTrap() and the
/// core call ends with carrierTrap().
using BuiltinFactory = BuiltinFunction (*)(BuiltinBinding const& binding);

/// One row of BuiltinHandlerTable: a built-in, whether P11 runs it, and what makes its core function.
struct BuiltinHandlerRow
{
    binary::CanonBuiltin builtin; ///< The built-in; equal to its enumerator, past Lift and Lower.
    BuiltinSupport support;       ///< Whether P11 runs it.
    BuiltinFactory factory;       ///< What makes its core function; null when Unimplemented.
};

/// The core function of `resource.new`: (i32 rep) -> (i32 index).
[[nodiscard]] BuiltinFunction makeResourceNew(BuiltinBinding const& binding);
/// The core function of `resource.drop`: (i32 index) -> ().
[[nodiscard]] BuiltinFunction makeResourceDrop(BuiltinBinding const& binding);
/// The core function of `resource.rep`: (i32 index) -> (i32 rep).
[[nodiscard]] BuiltinFunction makeResourceRep(BuiltinBinding const& binding);

/// The first built-in BuiltinHandlerTable holds: `lift` and `lower` are not built-ins of their own but
/// how functions cross (Task 15).
inline constexpr auto FirstHandledBuiltin = binary::CanonBuiltin::ResourceNew;

/// A row of BuiltinHandlerTable that P12 implements.
/// @param builtin The built-in.
/// @return Its row.
[[nodiscard]] consteval BuiltinHandlerRow unimplemented(binary::CanonBuiltin builtin) noexcept
{
    return BuiltinHandlerRow { .builtin = builtin,
                               .support = BuiltinSupport::Unimplemented,
                               .factory = nullptr };
}

/// Every canonical built-in past `lift` and `lower`, in enumerator order, with what runs it.
inline constexpr auto BuiltinHandlerTable = std::to_array<BuiltinHandlerRow>({
    { .builtin = binary::CanonBuiltin::ResourceNew,
      .support = BuiltinSupport::Implemented,
      .factory = &makeResourceNew },
    { .builtin = binary::CanonBuiltin::ResourceDrop,
      .support = BuiltinSupport::Implemented,
      .factory = &makeResourceDrop },
    { .builtin = binary::CanonBuiltin::ResourceRep,
      .support = BuiltinSupport::Implemented,
      .factory = &makeResourceRep },
    unimplemented(binary::CanonBuiltin::BackpressureInc),
    unimplemented(binary::CanonBuiltin::BackpressureDec),
    unimplemented(binary::CanonBuiltin::TaskReturn),
    unimplemented(binary::CanonBuiltin::TaskCancel),
    unimplemented(binary::CanonBuiltin::ContextGet),
    unimplemented(binary::CanonBuiltin::ContextSet),
    unimplemented(binary::CanonBuiltin::SubtaskCancel),
    unimplemented(binary::CanonBuiltin::SubtaskDrop),
    unimplemented(binary::CanonBuiltin::StreamNew),
    unimplemented(binary::CanonBuiltin::StreamRead),
    unimplemented(binary::CanonBuiltin::StreamWrite),
    unimplemented(binary::CanonBuiltin::StreamCancelRead),
    unimplemented(binary::CanonBuiltin::StreamCancelWrite),
    unimplemented(binary::CanonBuiltin::StreamDropReadable),
    unimplemented(binary::CanonBuiltin::StreamDropWritable),
    unimplemented(binary::CanonBuiltin::StreamForward),
    unimplemented(binary::CanonBuiltin::FutureNew),
    unimplemented(binary::CanonBuiltin::FutureRead),
    unimplemented(binary::CanonBuiltin::FutureWrite),
    unimplemented(binary::CanonBuiltin::FutureCancelRead),
    unimplemented(binary::CanonBuiltin::FutureCancelWrite),
    unimplemented(binary::CanonBuiltin::FutureDropReadable),
    unimplemented(binary::CanonBuiltin::FutureDropWritable),
    unimplemented(binary::CanonBuiltin::FutureForward),
    unimplemented(binary::CanonBuiltin::ErrorContextNew),
    unimplemented(binary::CanonBuiltin::ErrorContextDebugMessage),
    unimplemented(binary::CanonBuiltin::ErrorContextDrop),
    unimplemented(binary::CanonBuiltin::WaitableSetNew),
    unimplemented(binary::CanonBuiltin::WaitableSetWait),
    unimplemented(binary::CanonBuiltin::WaitableSetPoll),
    unimplemented(binary::CanonBuiltin::WaitableSetDrop),
    unimplemented(binary::CanonBuiltin::WaitableJoin),
    unimplemented(binary::CanonBuiltin::ThreadIndex),
    unimplemented(binary::CanonBuiltin::ThreadNewIndirect),
    unimplemented(binary::CanonBuiltin::ThreadResumeLater),
    unimplemented(binary::CanonBuiltin::ThreadSuspend),
    unimplemented(binary::CanonBuiltin::ThreadYield),
    unimplemented(binary::CanonBuiltin::ThreadSuspendThenResume),
    unimplemented(binary::CanonBuiltin::ThreadYieldThenResume),
    unimplemented(binary::CanonBuiltin::ThreadSuspendThenPromote),
    unimplemented(binary::CanonBuiltin::ThreadYieldThenPromote),
    unimplemented(binary::CanonBuiltin::ThreadSpawnRef),
    unimplemented(binary::CanonBuiltin::ThreadSpawnIndirect),
    unimplemented(binary::CanonBuiltin::ThreadAvailableParallelism),
});

static_assert(
    [] {
        auto expected = static_cast<std::size_t>(FirstHandledBuiltin);
        for (auto const& row: BuiltinHandlerTable)
        {
            if (static_cast<std::size_t>(row.builtin) != expected
                || (row.support == BuiltinSupport::Implemented) != (row.factory != nullptr))
                return false;
            ++expected;
        }
        return expected == static_cast<std::size_t>(binary::LastCanonBuiltin) + 1;
    }(),
    "BuiltinHandlerTable must hold one row per built-in past lift and lower, in enumerator order, "
    "with a factory exactly when Implemented");

/// The row of @p builtin.
/// @param builtin A built-in past `lift` and `lower` (checked).
/// @return Its BuiltinHandlerTable row.
[[nodiscard]] BuiltinHandlerRow const& builtinHandlerRow(binary::CanonBuiltin builtin) noexcept;

} // namespace isoline::component::runtime
