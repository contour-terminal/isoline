// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Limits.hpp>

namespace isoline::runtime
{

namespace detail
{
    /// The widest row of binary::MemoryAccessTable: every access an executor reads at a masked offset, the
    /// v128 accesses' 16 bytes among them.
    [[nodiscard]] consteval uint64_t widestMaskedAccess() noexcept
    {
        auto widest = uint64_t { 0 };
        for (auto const& access: binary::MemoryAccessTable)
            widest = std::max<uint64_t>(widest, access.width);
        return widest;
    }
} // namespace detail

/// The widest access an executor makes at an offset its Spectre mask chose (fastinterpreter accessMask()): on
/// a mispredicted bounds check it reads this many bytes at a memory's base, so a memory of fewer bytes is
/// read past its end, speculatively: MaxAccessWidth bytes past the end of a memory of none. Every memory
/// keeps the difference readable, or as a guard, after its end (requiredTailBytesOf()).
inline constexpr auto MaxAccessWidth = detail::widestMaskedAccess();

/// A linear memory's storage: the bytes a memory instance reads and writes, wherever they live.
///
/// The store allocates one per memory through the embedder's IMemoryAllocator and reaches its bytes
/// only through bytes(), which a grow may move: whoever holds a span re-fetches it after every grow
/// and every host call, which may grow.
class ILinearMemory
{
  public:
    virtual ~ILinearMemory() = default;

    /// The accessible bytes: the memory's size, a whole number of its pages.
    /// @return Its bytes; valid until the next grow().
    [[nodiscard]] virtual std::span<std::byte> bytes() noexcept = 0;

    /// @copydoc bytes()
    [[nodiscard]] virtual std::span<std::byte const> bytes() const noexcept = 0;

    /// How many inaccessible bytes follow bytes().end(): the guard region an access past the end
    /// faults in rather than reaching other memory. 0 when there is none.
    ///
    /// The store checks it against StoreServices::requiredGuardBytes once, at allocation, and relies
    /// on it from then on: the guard must hold after every grow(), behind the grown bytes, as long as
    /// the memory lives. Memories that executors bounds-check explicitly, memory64 and custom page
    /// sizes (spec §7), need no guard and are to be exempt from requiredGuardBytes; the store does not
    /// exempt them yet, which the compiling executors that rely on the guard enforce (P7).
    /// @return The guard's size in bytes.
    [[nodiscard]] virtual uint64_t guardBytes() const noexcept = 0;

    /// How many readable bytes the memory's storage holds after bytes().end(), which no access reaches
    /// but a speculative one may read (MaxAccessWidth): bytes the memory owns, never the guest's or another
    /// object's. With guardBytes() they make the tail, which must be at least
    /// requiredTailBytesOf(bytes().size()) at every size the memory has: when it is allocated, which the
    /// store checks against MemoryRequest::requiredTailBytes and refuses with Guard, and after every grow(),
    /// which the store checks too (a contract violation). A tail that shrinks by what a grow adds, as a fixed
    /// buffer's does, keeps the rule.
    /// @return The readable bytes past the end; 0 when there are none.
    [[nodiscard]] virtual uint64_t tailBytes() const noexcept = 0;

    /// Grows the memory to @p newBytes, the added bytes zeroed. The store calls it only within the
    /// request's maximumBytes and the store's budget, never to shrink, and charges the budget only
    /// when this succeeds.
    /// @param newBytes The new size in bytes, at least the current one.
    /// @return Nothing; or, the memory unchanged, why it was refused: Embedder when the embedder
    ///         declines it, HostMemory when the host has no memory for it. Any other refusal is the
    ///         store's alone to decide, so the store reports it as Embedder.
    [[nodiscard]] virtual std::expected<void, Refusal> grow(uint64_t newBytes) noexcept = 0;
};

/// What the store asks an IMemoryAllocator for: one memory, its size now and the most it may grow to.
struct MemoryRequest
{
    binary::MemoryType type {}; ///< The memory's type, as the module declares it.
    uint64_t initialBytes = 0;  ///< Its size to begin with: its minimum pages, in bytes.
    /// The most it may grow to: its maximum, capped by its address type's reach and by the store's
    /// StoreLimits::maxMemoryBytes, rounded down to whole pages. An allocator reserving address space
    /// needs no more.
    uint64_t maximumBytes = 0;
    /// The guard the store requires after the accessible bytes (StoreServices::requiredGuardBytes); a
    /// memory whose guardBytes() is smaller is refused with Guard.
    uint64_t requiredGuardBytes = 0;
    /// The bytes past the end a memory of initialBytes must keep, readable (ILinearMemory::tailBytes()) or as
    /// a guard (ILinearMemory::guardBytes()), together: requiredTailBytesOf(initialBytes), what a speculative
    /// access at its base reads past its end (MaxAccessWidth). A memory whose tail and guard together are
    /// smaller is refused with Guard.
    uint64_t requiredTailBytes = 0;
};

/// The tail a memory of @p size bytes must keep after its end, readable or as a guard: what an access of
/// MaxAccessWidth bytes at its base reads past it. Whatever its page size: a memory of no pages has no bytes.
/// @param size The memory's size in bytes.
/// @return MaxAccessWidth less @p size, or 0 where the memory holds MaxAccessWidth bytes or more.
[[nodiscard]] constexpr uint64_t requiredTailBytesOf(uint64_t size) noexcept
{
    return MaxAccessWidth - std::min(size, MaxAccessWidth);
}

/// Whether @p memory keeps the tail its size needs (requiredTailBytesOf()), readable or guarded.
/// @param memory The memory.
/// @return Whether its tail and guard together are at least that.
[[nodiscard]] inline bool keepsItsTail(ILinearMemory const& memory) noexcept
{
    auto const required = requiredTailBytesOf(memory.bytes().size());
    auto const tail = memory.tailBytes();
    return tail >= required || memory.guardBytes() >= required - tail;
}

/// Allocates the store's linear memories: the seam an embedder provides its own storage through.
class IMemoryAllocator
{
  public:
    virtual ~IMemoryAllocator() = default;

    /// Allocates one memory of @p request's initialBytes, zeroed.
    /// @param request What the memory is and may become.
    /// @return The memory; or why it was refused: Embedder when the embedder declines it, HostMemory
    ///         when the host has no memory for it, Guard when it cannot provide the request's
    ///         requiredGuardBytes or requiredTailBytes. Any other refusal is the store's alone to decide, so
    ///         the store reports it as Embedder. A memory must not be null (checked).
    [[nodiscard]] virtual std::expected<std::unique_ptr<ILinearMemory>, Refusal> allocate(
        MemoryRequest const& request) noexcept = 0;
};

/// The default allocator: each memory a std::vector on the heap, with no guard region (guardBytes()
/// is 0), so a store requiring a guard refuses it, and MaxAccessWidth zeroed bytes after its end at every
/// size (tailBytes()), so that a speculative access at a small memory's base, even of no bytes, reads only
/// them. A grow resizes the vector and zero-fills the added bytes in one unpolled host operation, bounded by
/// StoreLimits::maxMemoryBytes; std::bad_alloc, or a size past what a vector can hold, is refused with
/// HostMemory.
class HeapMemoryAllocator final: public IMemoryAllocator
{
  public:
    /// @copydoc IMemoryAllocator::allocate
    [[nodiscard]] std::expected<std::unique_ptr<ILinearMemory>, Refusal> allocate(
        MemoryRequest const& request) noexcept override;
};

} // namespace isoline::runtime
