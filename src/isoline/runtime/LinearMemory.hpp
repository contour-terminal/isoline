// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Limits.hpp>

namespace isoline::runtime
{

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
};

/// Allocates the store's linear memories: the seam an embedder provides its own storage through.
class IMemoryAllocator
{
  public:
    virtual ~IMemoryAllocator() = default;

    /// Allocates one memory of @p request's initialBytes, zeroed.
    /// @param request What the memory is and may become.
    /// @return The memory; or why it was refused: Embedder when the embedder declines it, HostMemory
    ///         when the host has no memory for it, Guard when it cannot provide the request's
    ///         requiredGuardBytes. Any other refusal is the store's alone to decide, so the store
    ///         reports it as Embedder. A memory must not be null (checked).
    [[nodiscard]] virtual std::expected<std::unique_ptr<ILinearMemory>, Refusal> allocate(
        MemoryRequest const& request) noexcept = 0;
};

/// The default allocator: each memory a std::vector on the heap, with no guard region (guardBytes()
/// is 0), so a store requiring a guard refuses it. A grow resizes the vector and zero-fills the added
/// bytes in one unpolled host operation, bounded by StoreLimits::maxMemoryBytes; std::bad_alloc, or a
/// size past what a vector can hold, is refused with HostMemory.
class HeapMemoryAllocator final: public IMemoryAllocator
{
  public:
    /// @copydoc IMemoryAllocator::allocate
    [[nodiscard]] std::expected<std::unique_ptr<ILinearMemory>, Refusal> allocate(
        MemoryRequest const& request) noexcept override;
};

} // namespace isoline::runtime
