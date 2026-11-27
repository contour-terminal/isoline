// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <isoline/api/Types.hpp>
#include <isoline/component/canonical/GuestMemory.hpp>
#include <isoline/component/canonical/Layout.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::canonical
{

/// The handle operations lifting and lowering an `own` or a `borrow` takes (definitions.py's
/// lift_own, lift_borrow, lower_own and lower_borrow), on the handle table of the instance whose side
/// of the call is lifted or lowered. Task 13's ResourceOperations implements them; tests use a fake.
///
/// @p type is always a resource of the LiftContext's arena, the side's own view of it: the
/// implementation maps it to the runtime ResourceTypeIndex that instance bound it to.
class HandleOps
{
  public:
    virtual ~HandleOps() = default;

    /// lift_own: removes the own handle at @p index.
    /// @param type The resource type the value's type names.
    /// @param index The handle index the guest passed.
    /// @return The resource it held; or UnknownHandle, WrongHandleType, LendOutstanding, or a borrow
    ///         where an own is expected.
    [[nodiscard]] virtual std::expected<ResourceValue, Trap> liftOwn(types::ResourceTypeId type,
                                                                     uint32_t index) = 0;

    /// lift_borrow: the handle at @p index, lent to the current call.
    /// @param type The resource type the value's type names.
    /// @param index The handle index the guest passed.
    /// @return The resource it names, as a borrow; or UnknownHandle or WrongHandleType.
    [[nodiscard]] virtual std::expected<ResourceValue, Trap> liftBorrow(types::ResourceTypeId type,
                                                                        uint32_t index) = 0;

    /// lower_own: adds an own handle of @p value to the table.
    /// @param type The resource type the destination's type names.
    /// @param value The resource.
    /// @return Its new handle index; or Handles when the store's handles are exhausted.
    [[nodiscard]] virtual std::expected<uint32_t, Trap> lowerOwn(types::ResourceTypeId type,
                                                                 ResourceValue value) = 0;

    /// lower_borrow: the representation, when the destination implements the resource; else a borrow
    /// handle added to the table for the current call.
    /// @param type The resource type the destination's type names.
    /// @param value The resource.
    /// @return The core value to pass; or Handles when the store's handles are exhausted.
    [[nodiscard]] virtual std::expected<uint32_t, Trap> lowerBorrow(types::ResourceTypeId type,
                                                                    ResourceValue value) = 0;
};

/// What one side of a canonical lift or lower works with: its types and their layouts, its memory and
/// string encoding (the canonical options), the store's live-bytes budget, and its handle table.
struct LiftContext
{
    types::TypeArena const& arena;   ///< The side's types: every TypeId given is of it.
    std::span<Layout const> layouts; ///< The layouts of @ref arena's types, by TypeId.
    /// The side's memory; null without a `memory` option, which validation allows only where no value
    /// reaches memory (checked).
    IGuestMemoryAccess* memory;
    StringEncoding encoding; ///< The side's `string-encoding` option (Task 12).
    /// The budget lifted values are charged to: the store's.
    std::shared_ptr<LiveBytesBudget> budget;
    HandleOps& handles; ///< The side's handle table.
};

/// Lifts values of @p types from core values (definitions.py's lift_flat_values): flat when their
/// flattening has at most @p maxFlat core values, else from the tuple @p flat's one pointer points at.
///
/// Integers wrap to their width, a bool is any nonzero value, a float's NaN is canonicalized, a `char`
/// traps past 0x10FFFF and on surrogates, unknown flag bits are ignored, and a variant's payload is
/// read from the joined core values its cases share, bit-cast to what the case holds. Every value is
/// charged to the context's budget as it is built.
/// @param context The lifting side.
/// @param types The values' types, of the context's arena.
/// @param flat The core values: the flattening of @p types, or one i32 pointer (checked).
/// @param maxFlat MaxFlatParams, MaxFlatResults or MaxFlatAsyncParams (checked: at most MaxFlatParams).
/// @return The values, one root per type; or the first trap.
/// @throws std::bad_alloc When the host cannot allocate what the budget allowed.
[[nodiscard]] std::expected<Values, Trap> liftFlat(LiftContext& context,
                                                   std::span<types::TypeId const> types,
                                                   std::span<isoline::api::Value const> flat,
                                                   uint32_t maxFlat);

/// Lifts values of @p types from memory, laid out as a tuple of them at @p address (definitions.py's
/// load of a TupleType): the address must be aligned to the tuple and the tuple lie in memory.
/// @param context The lifting side; it has a memory (checked).
/// @param types The values' types, of the context's arena.
/// @param address The tuple's address.
/// @return The values, one root per type; or the first trap.
/// @throws std::bad_alloc When the host cannot allocate what the budget allowed.
[[nodiscard]] std::expected<Values, Trap> load(LiftContext& context,
                                               std::span<types::TypeId const> types,
                                               uint32_t address);

/// Lowers @p values as @p types into core values (definitions.py's lower_flat_values): flat when the
/// flattening has at most @p maxFlat core values, else into a tuple in memory, at @p resultPointer
/// when given (a lowered function's results), else in a block the context's `realloc` allocates, whose
/// address is the one core value returned.
///
/// Walks the destination types and the source values side by side (D9): the values may come from
/// another arena, and are read by structure, never by their TypeIds. A NaN is canonicalized.
/// @param context The lowering side, whose types @p types are.
/// @param types The destination types.
/// @param values One value per type, of the same structure (checked).
/// @param maxFlat MaxFlatParams, MaxFlatResults or MaxFlatAsyncParams (checked: at most MaxFlatParams).
/// @param resultPointer Where a tuple that does not fit flat goes; empty to allocate it.
/// @return The core values; empty when written at @p resultPointer; or the first trap.
/// @throws std::bad_alloc When the core values cannot be stored.
[[nodiscard]] std::expected<std::vector<isoline::api::Value>, Trap> lowerFlat(
    LiftContext& context,
    std::span<types::TypeId const> types,
    std::span<ValueView const> values,
    uint32_t maxFlat,
    std::optional<uint32_t> resultPointer);

/// Stores @p value as @p type at @p address (definitions.py's store): the address must be aligned to
/// the type and the value lie in memory.
/// @param context The lowering side; it has a memory (checked).
/// @param type The destination type, of the context's arena.
/// @param value A value of the same structure (checked).
/// @param address Where it goes.
/// @return Nothing, or the first trap.
/// @throws std::bad_alloc When a trap's message cannot be stored.
[[nodiscard]] std::expected<void, Trap> store(LiftContext& context,
                                              types::TypeId type,
                                              ValueView value,
                                              uint32_t address);

} // namespace isoline::component::canonical
