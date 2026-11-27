// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <memory_resource>
#include <type_traits>
#include <vector>

namespace isoline::fastinterpreter
{

/// The allocator of every container of lowering and of lowered code: what the container allocates for its
/// own storage goes through the lowering arena, charged to the compile budget and refusable; what a standard
/// library allocates beside it for bookkeeping goes around the budget, to the process heap.
///
/// A container allocates its storage under its own element type, @p Charged, and only in operations that
/// may throw (reserve, push_back, a copy), so a refusal of the arena reaches lowering as std::bad_alloc at a
/// point that handles it. A standard library may also allocate bookkeeping under a type it rebinds the
/// allocator to: MSVC's debug containers (_ITERATOR_DEBUG_LEVEL above 0) allocate a proxy of two pointers in
/// their allocator-taking constructor and in their moves, which are noexcept. A refusal there would end the
/// process, and a charge there would make the budget differ by standard library, so an allocation of any type
/// but @p Charged is neither charged nor refused: it comes from std::pmr::new_delete_resource(), the
/// documented path around the budget. libstdc++ and libc++ make no such allocation, so every byte the
/// arena counts on any of the three is one the code asked for.
///
/// @tparam T The type it allocates, as the container rebinds it.
/// @tparam Charged The container's element type, whose allocations the arena charges. Only a container that
///         allocates its storage as its element type may use it, a vector's kind: a node-based container's
///         nodes are rebound types too, which no allocator tells from bookkeeping (IndexMap is lowering's
///         map).
template <typename T, typename Charged = T>
class LoweringAllocator
{
  public:
    using value_type = T; // NOLINT(readability-identifier-naming): the Allocator requirements name it so.

    /// An allocator drawing on @p arena. Implicit, as std::pmr::polymorphic_allocator's is, so that a
    /// container is constructed from the arena alone.
    /// @param arena The lowering arena.
    LoweringAllocator(std::pmr::memory_resource* arena) noexcept: _arena { arena } {}

    /// The allocator of the same arena and the same container, rebound to @p U's allocations.
    template <typename U>
    LoweringAllocator(LoweringAllocator<U, Charged> const& other) noexcept: _arena { other.resource() }
    {
    }

    /// Storage for @p count values of T: from the arena for the container's own storage, otherwise from the
    /// process heap.
    /// @param count The values.
    /// @return The storage.
    [[nodiscard]] T* allocate(std::size_t count)
    {
        return static_cast<T*>(source()->allocate(count * sizeof(T), alignof(T)));
    }

    /// Gives back what allocate() gave for @p count values.
    /// @param pointer The storage.
    /// @param count The values.
    void deallocate(T* pointer, std::size_t count) noexcept
    {
        source()->deallocate(pointer, count * sizeof(T), alignof(T));
    }

    /// The lowering arena.
    /// @return The arena.
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept { return _arena; }

    /// Whether both draw on the same arena: what one allocated, the other may free.
    [[nodiscard]] friend bool operator==(LoweringAllocator const& left,
                                         LoweringAllocator const& right) noexcept
    {
        return left._arena == right._arena;
    }

  private:
    /// Whether this allocator's allocations are its container's own storage.
    static constexpr auto IsStorage = std::is_same_v<T, Charged>;

    /// Where its allocations come from: the arena for the container's own storage, the process heap for a
    /// standard library's bookkeeping.
    [[nodiscard]] std::pmr::memory_resource* source() const noexcept
    {
        if constexpr (IsStorage)
            return _arena;
        else
            return std::pmr::new_delete_resource();
    }

    std::pmr::memory_resource* _arena;
};

/// A vector of lowering or of lowered code.
template <typename T>
using LoweringVector = std::vector<T, LoweringAllocator<T>>;

} // namespace isoline::fastinterpreter
