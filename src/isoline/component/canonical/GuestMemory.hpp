// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <isoline/component/canonical/Trap.hpp>

namespace isoline::component::canonical
{

/// A component instance's linear memory and its `realloc`, as lifting and lowering reach them: by
/// copies only. No view of guest memory outlives one call of this interface, because a `realloc` (or any
/// guest code) may grow the memory and move it; and every read or write is bounds-checked against the
/// memory's size at that moment. Task 15's GuestMemory implements it through the store (A2); tests use
/// a fake (canonical/testing/GuestDoubles.hpp).
class IGuestMemoryAccess
{
  public:
    virtual ~IGuestMemoryAccess() = default;

    /// Copies `out.size()` bytes from @p address into @p out, once.
    /// @param address The first byte's address.
    /// @param out Receives the bytes.
    /// @return Nothing, or OutOfBounds when a byte lies past the memory's end (nothing copied).
    [[nodiscard]] virtual std::expected<void, Trap> read(uint32_t address, std::span<std::byte> out) = 0;

    /// Copies @p bytes into the memory at @p address.
    /// @param address The first byte's address.
    /// @param bytes The bytes.
    /// @return Nothing, or OutOfBounds when a byte lies past the memory's end (nothing written).
    [[nodiscard]] virtual std::expected<void, Trap> write(uint32_t address,
                                                          std::span<std::byte const> bytes) = 0;

    /// Calls the instance's `cabi_realloc` with `may_leave` cleared.
    /// @param old The block to resize, or 0 for a new one.
    /// @param oldSize Its size, or 0.
    /// @param alignment The alignment the result must have.
    /// @param newSize The size wanted.
    /// @return The block's address; or a trap: the guest's, CannotLeave, UnalignedPointer or OutOfBounds
    ///         for a misaligned, out-of-bounds or wrapping result, CannotEnter when the store locked
    ///         down meanwhile.
    [[nodiscard]] virtual std::expected<uint32_t, Trap> reallocate(uint32_t old,
                                                                   uint32_t oldSize,
                                                                   uint32_t alignment,
                                                                   uint32_t newSize) = 0;

    /// Polls the store (Store::poll(), PollSite::HostBulk) for one api::BulkPollBytes granule of host
    /// work: lifting and lowering call it after every api::BulkPollBytes bytes they copy.
    /// @return Nothing, or Interrupted, or the core trap the step charge ended in.
    [[nodiscard]] virtual std::expected<void, Trap> poll() = 0;

    /// The memory's size now, asked afresh each time: a `realloc` may have grown it.
    /// @return Its size in bytes.
    [[nodiscard]] virtual uint64_t size() = 0;
};

/// The trap of a range of guest memory past the memory's end.
/// @param address The range's first byte.
/// @param length Its length.
/// @param memorySize The memory's size.
/// @return OutOfBounds, naming the three.
/// @throws std::bad_alloc When the trap's message cannot be stored.
[[nodiscard]] Trap outOfBounds(uint32_t address, uint64_t length, uint64_t memorySize);

/// Checks that @p length bytes at @p address are aligned to @p alignment and lie in a memory of
/// @p memorySize bytes (definitions.py's two trap_if of lift_flat_values and lower_flat_values), in
/// 64-bit arithmetic, so no range wraps.
/// @param address The range's first byte.
/// @param length Its length.
/// @param alignment The alignment @p address must have, a power of two (checked).
/// @param memorySize The memory's size.
/// @return Nothing, or UnalignedPointer, or OutOfBounds; alignment is checked first.
/// @throws std::bad_alloc When the trap's message cannot be stored.
[[nodiscard]] std::expected<void, Trap> checkRange(uint32_t address,
                                                   uint64_t length,
                                                   uint32_t alignment,
                                                   uint64_t memorySize);

/// definitions.py's LiftLowerContext.reallocate: calls @p memory's `realloc`, then traps on a result not
/// aligned to @p alignment or whose @p newSize bytes pass the memory's end, its size asked after the
/// call, which may have grown it; in 64-bit arithmetic, so no result wraps.
/// @param memory The memory and its `realloc`.
/// @param old The block to resize, or 0 for a new one.
/// @param oldSize Its size, or 0.
/// @param alignment The alignment the result must have, a power of two (checked).
/// @param newSize The size wanted.
/// @return The block's address; or the trap of the call, UnalignedPointer, or OutOfBounds.
/// @throws std::bad_alloc When a trap's message cannot be stored.
[[nodiscard]] std::expected<uint32_t, Trap> reallocate(
    IGuestMemoryAccess& memory, uint32_t old, uint32_t oldSize, uint32_t alignment, uint32_t newSize);

} // namespace isoline::component::canonical
