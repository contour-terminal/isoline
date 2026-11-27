// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <ranges>
#include <variant>
#include <vector>

#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>

namespace isoline::component::runtime
{

using canonical::Ownership;
using canonical::ResourceTypeIndex;
using canonical::Trap;

/// A task, as a borrow handle names the call it was lent to (definitions.py's borrow_scope): a serial
/// TaskStack hands out, never reused, so a handle naming a task that has ended names no task at all.
struct TaskId
{
    uint64_t value = 0; ///< The serial; 0 names no task.

    /// Ids compare by their serial.
    auto operator<=>(TaskId const&) const = default;
};

/// A handle table's slot that holds nothing: never allocated past, or freed.
struct FreeSlot
{
    /// Free slots are all alike.
    bool operator==(FreeSlot const&) const = default;
};

/// A resource handle (definitions.py's ResourceHandle): which resource, whether it owns it, the call a
/// borrow was lent to, and how many calls in progress borrow it from here.
struct ResourceHandle
{
    ResourceTypeIndex type {};            ///< Its runtime resource type.
    uint32_t rep = 0;                     ///< The resource's representation.
    Ownership ownership = Ownership::Own; ///< Whether it owns the resource.
    TaskId borrowScope {};                ///< For a borrow, the task it was lent to; else none.
    uint32_t lendCount = 0;               ///< The calls in progress it is lent to (num_lends).

    /// Two handles are equal when every member is.
    bool operator==(ResourceHandle const&) const = default;
};

/// What a handle table's slot holds. P12 appends Waitable, WaitableSet, Subtask, StreamEnd, FutureEnd
/// and ErrorContext.
using HandleEntry = std::variant<FreeSlot, ResourceHandle>;

/// The greatest index one handle table hands out (definitions.py's Table.MAX_LENGTH, which an add()
/// past traps): an index fits 28 bits, so P12 packs it beside a 4-bit state.
inline constexpr auto HandleTableMaxLength = (uint32_t { 1 } << 28U) - 1;

/// The slots of a table's first chunk; each further chunk holds twice its predecessor's.
inline constexpr auto HandleTableFirstChunkLength = uint32_t { 64 };

/// The handle slots of a store, every handle table's together (ComponentLimits::maxHandles), plus the
/// handles lent to the calls in progress (Subtask::addLender()).
///
/// A table charges a slot when it grows and keeps it, freed or not, until it is destroyed: a freed
/// slot is the table's to reuse (LIFO, as definitions.py's free list), so a table's memory is what its
/// slots charged, and the store's tables together hold at most the limit's slots, however handles move
/// between them. Not thread-safe: one store runs one call at a time.
class HandleBudget
{
  public:
    /// A budget holding nothing.
    /// @param limit The most slots and lends it holds at once.
    explicit HandleBudget(uint32_t limit) noexcept: _limit { limit } {}

    HandleBudget(HandleBudget const&) = delete;
    HandleBudget& operator=(HandleBudget const&) = delete;
    HandleBudget(HandleBudget&&) = delete;
    HandleBudget& operator=(HandleBudget&&) = delete;
    ~HandleBudget() = default;

    /// Charges one slot or lend.
    /// @return Nothing, or the Handles trap when the budget is at its limit; a refused charge changes
    ///         nothing.
    /// @throws std::bad_alloc When the trap's message cannot be stored.
    [[nodiscard]] std::expected<void, Trap> charge();

    /// Releases slots or lends charged before.
    /// @param count At most what is used (checked).
    void release(uint32_t count) noexcept;

    /// The slots and lends charged and not released.
    /// @return The count.
    [[nodiscard]] uint32_t used() const noexcept { return _used; }

    /// The most slots and lends the budget holds.
    /// @return The limit it was made with.
    [[nodiscard]] uint32_t limit() const noexcept { return _limit; }

  private:
    uint32_t _limit;
    uint32_t _used = 0;
};

/// One component instance's handle table (definitions.py's Table): slot 0 is never a handle; a new
/// handle takes the slot freed last (a LIFO free list), else a new slot at the end. Indices are what the
/// guest sees, so they follow definitions.py exactly.
///
/// Entries live in chunks that never move: chunk k holds HandleTableFirstChunkLength << k slots, so an
/// entry's address is stable across add(). Still, nothing holds an entry across an add(): a reference
/// get() returns is for the one operation at hand, and anything kept names a handle by (instance,
/// index) (Lender). Every slot the table grows by is charged to the store's HandleBudget, and released
/// when the table is destroyed.
class HandleTable
{
  public:
    /// An empty table: one slot, 0, which is never a handle and costs nothing.
    /// @param budget The store's budget; it outlives the table.
    /// @param maxIndex The greatest index it hands out: HandleTableMaxLength, or less (checked) for a
    ///        test that reaches it.
    explicit HandleTable(HandleBudget& budget, uint32_t maxIndex = HandleTableMaxLength) noexcept;

    HandleTable(HandleTable const&) = delete;
    HandleTable& operator=(HandleTable const&) = delete;
    HandleTable(HandleTable&&) = delete;
    HandleTable& operator=(HandleTable&&) = delete;
    /// Releases every slot the table charged.
    ~HandleTable();

    /// Adds @p entry: at the slot freed last, else at a new slot past the end.
    /// @param entry A handle (checked: not a FreeSlot).
    /// @return Its index, never 0; or Handles when a new slot's index would pass the table's greatest,
    ///         or its slot the store's budget. A refused add changes nothing.
    /// @throws std::bad_alloc When a new chunk cannot be allocated; the table is unchanged.
    [[nodiscard]] std::expected<uint32_t, Trap> add(HandleEntry entry);

    /// The handle at @p index, for the operation at hand only: never kept across an add().
    /// @param index A guest's handle index.
    /// @return It; or UnknownHandle for 0, an index past the end, or a free slot.
    /// @throws std::bad_alloc When the trap's message cannot be stored.
    [[nodiscard]] std::expected<std::reference_wrapper<HandleEntry>, Trap> get(uint32_t index);

    /// Removes the handle at @p index, freeing its slot for the next add().
    /// @param index A guest's handle index.
    /// @return The handle it held; or UnknownHandle as get() reports it, the table unchanged.
    /// @throws std::bad_alloc When the free list cannot grow; the table is unchanged.
    [[nodiscard]] std::expected<HandleEntry, Trap> remove(uint32_t index);

    /// The table's slots, slot 0 and free slots included.
    /// @return The length definitions.py's array would have.
    [[nodiscard]] uint32_t length() const noexcept { return _length; }

    /// Calls @p visit with each slot's index and entry, free slots included, in index order: for a
    /// store's teardown, which runs no guest code.
    /// @param visit Called as visit(uint32_t index, HandleEntry&); it must not add to the table.
    template <typename Visit>
    void forEachSlot(Visit visit)
    {
        for (auto const index: std::views::iota(uint32_t { 1 }, _length))
            visit(index, slot(index));
    }

  private:
    /// The chunks index HandleTableMaxLength needs: chunks 0 to 22 hold 64 x (2^23 - 1) slots, past 2^28.
    static constexpr auto ChunkCount = std::size_t { 23 };

    /// The slot @p index (checked: below the length).
    [[nodiscard]] HandleEntry& slot(uint32_t index) noexcept;

    HandleBudget* _budget;
    uint32_t _maxIndex;
    uint32_t _length = 1;
    /// The chunks, each sized once when the table first grows into it and never resized after, so its
    /// entries never move.
    std::array<std::vector<HandleEntry>, ChunkCount> _chunks {};
    std::vector<uint32_t> _free {};
};

} // namespace isoline::component::runtime
