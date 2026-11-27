// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
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

/// Counts the bytes one canonical call copies, transcodes or reads back through guest memory, and polls
/// the store for every api::BulkPollBytes of them (Global Constraints, interrupt latency): one counter
/// for the whole call, so strings and lists copied by separate loads and stores still poll on time.
class BulkPolls
{
  public:
    /// Counts @p bytes of host work, polling once for each whole granule they complete.
    /// @param memory The memory whose poll() reaches the store.
    /// @param bytes The bytes.
    /// @return Nothing, or the trap a poll ended in (Interrupted, or the step charge's).
    [[nodiscard]] std::expected<void, Trap> copied(IGuestMemoryAccess& memory, uint64_t bytes);

  private:
    uint64_t _sincePoll = 0; ///< The bytes counted since the last poll.
};

/// What one side of a canonical lift or lower works with: its types and their layouts, its memory and
/// string encoding (the canonical options), the store's live-bytes budget, and its handle table.
///
/// One per canonical call: its BulkPolls counts the bytes every lift and lower of the call copies.
struct LiftContext
{
    types::TypeArena const& arena;   ///< The side's types: every TypeId given is of it.
    std::span<Layout const> layouts; ///< The layouts of @ref arena's types, by TypeId.
    /// The side's memory; null without a `memory` option, which validation allows only where no value
    /// reaches memory (checked).
    IGuestMemoryAccess* memory;
    StringEncoding encoding; ///< The side's `string-encoding` option, which strings lift and lower in.
    /// The budget lifted values are charged to: the store's.
    std::shared_ptr<LiveBytesBudget> budget;
    HandleOps& handles; ///< The side's handle table.
    BulkPolls polls {}; ///< The bytes the call copied since its last poll.
};

/// The trap of a ValueBuilder's refusal during a lift: LiveValueBytes when the budget refused; any other
/// refusal means the lift built a value its type does not admit (checked).
/// @param context The lifting side, whose budget's limit the trap names.
/// @param error The builder's error.
/// @return LiveValueBytes.
/// @throws std::bad_alloc When the trap's message cannot be stored.
[[nodiscard]] Trap builderTrap(LiftContext const& context, ValueError const& error);

/// Writes bytes into a block of guest memory in order, through a buffer: one write() per BufferBytes,
/// and the context's BulkPolls counts every byte written. No view of guest memory is kept: a `realloc`
/// between two writes may move or grow the memory.
class BlockWriter
{
  public:
    /// The bytes one write() copies at most.
    static constexpr auto BufferBytes = std::size_t { 4096 };

    /// A writer at the start of a block.
    /// @param context The lowering side; it has a memory (checked).
    /// @param block The block's address, which a checked reallocate() returned.
    /// @param size Its size: no byte is written past it (checked).
    BlockWriter(LiftContext& context, uint32_t block, uint32_t size) noexcept;

    /// Appends @p bytes.
    /// @param bytes The bytes.
    /// @return Nothing, or the trap a write or a poll ended in.
    [[nodiscard]] std::expected<void, Trap> put(std::span<std::byte const> bytes);

    /// Writes the bytes buffered.
    /// @return Nothing, or the trap the write or a poll ended in.
    [[nodiscard]] std::expected<void, Trap> flush();

    /// Continues at the same offset in the block a `realloc` resized this one to (checked: flushed).
    /// @param block The block's address.
    /// @param size Its size.
    void moveTo(uint32_t block, uint32_t size) noexcept;

    /// Continues at @p offset of the block (checked: flushed, and inside it).
    /// @param offset The offset.
    void seek(uint64_t offset) noexcept;

    /// The bytes appended since the block's start, buffered ones included.
    /// @return The offset the next byte goes to.
    [[nodiscard]] uint64_t offset() const noexcept { return _flushed + _filled; }

  private:
    LiftContext* _context;
    uint32_t _block;
    uint32_t _size;
    uint64_t _flushed = 0;   ///< The offset the buffer's first byte goes to.
    std::size_t _filled = 0; ///< The bytes buffered.
    std::array<std::byte, BufferBytes> _buffer {};
};

/// Lifts values of @p types from core values (definitions.py's lift_flat_values): flat when their
/// flattening has at most @p maxFlat core values, else from the tuple @p flat's one pointer points at.
///
/// Integers wrap to their width, a bool is any nonzero value, a float's NaN is canonicalized, a `char`
/// traps past 0x10FFFF and on surrogates, unknown flag bits are ignored, and a variant's payload is
/// read from the joined core values its cases share, bit-cast to what the case holds. A string or a
/// list is read from memory, its length and its range checked before anything is allocated for it
/// (Strings.hpp); a list of scalars is copied at once. Every value is charged to the context's budget
/// before it is built, and every api::BulkPollBytes copied polls the store.
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
/// another arena, and are read by structure, never by their TypeIds. A NaN is canonicalized. A string
/// or a list goes into a block the context's `realloc` allocates, in definitions.py's order of calls,
/// each result checked; its pointer pair is written once its elements are.
/// @param context The lowering side, whose types @p types are.
/// @param types The destination types.
/// @param values One value per type, of the same structure (checked).
/// @param maxFlat MaxFlatParams, MaxFlatResults or MaxFlatAsyncParams (checked: at most MaxFlatParams).
/// @param resultPointer Where a tuple that does not fit flat goes; empty to allocate it.
/// @return The core values; empty when written at @p resultPointer; or the first trap: a `realloc`'s,
///         a misaligned or out-of-bounds block's, a poll's, or a handle's.
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
