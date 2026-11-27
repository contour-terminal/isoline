// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <optional>

namespace isoline::runtime
{

/// Why a counting arena refused an allocation.
enum class ArenaRefusalReason : uint8_t
{
    Budget,     ///< It would have taken the arena past its budget: the module's compile budget.
    HostMemory, ///< The upstream resource could not provide it: the host is out of memory.
};

/// An allocation a counting arena refused.
struct ArenaRefusal
{
    ArenaRefusalReason reason = ArenaRefusalReason::Budget; ///< Why.
    uint64_t requested = 0;                                 ///< The bytes the allocation asked for.

    /// Whether both record the same refusal.
    [[nodiscard]] bool operator==(ArenaRefusal const&) const = default;
};

/// What a reservation a counting arena refused asked for, and what its budget had left.
struct ArenaBudgetExceeded
{
    uint64_t requested = 0; ///< The bytes the reservation asked for.
    uint64_t available = 0; ///< The bytes the budget had left beside what it had charged.

    /// Whether both record the same refusal.
    [[nodiscard]] bool operator==(ArenaBudgetExceeded const&) const = default;
};

/// The memory every container of a module's executable code allocates from, and the counter of the
/// module's compile budget, shared by every engine that prepares code at load.
///
/// A std::pmr::memory_resource that charges each allocation against a budget, and lets the engine
/// reserve a bound before it does the work the bound covers, so a module over the budget is refused
/// before its peak, not after it. Allocations draw on what is reserved first, then on the rest of the
/// budget. Past the budget, allocate() throws std::bad_alloc, as the memory_resource contract has it;
/// the engine turns that into its own error at its boundary (the fast interpreter's lower() into its
/// LoweringError), and lastRefusal() says why.
///
/// It is the injection seam of that memory: a prepared module keeps its arena as long as its code
/// lives. An arena is not safe to use from two threads at once; a module's arena allocates only while
/// the module is prepared, and frees when the module is destroyed.
class ICountingArena: public std::pmr::memory_resource
{
  public:
    /// The bytes the arena may charge at once.
    /// @return The budget.
    [[nodiscard]] virtual uint64_t budget() const noexcept = 0;

    /// The bytes charged now: every allocation not yet freed, and every reservation not yet drawn on.
    /// @return The charged bytes.
    [[nodiscard]] virtual uint64_t charged() const noexcept = 0;

    /// The bytes reserved and not yet drawn on by an allocation, which charged() includes.
    /// @return The reserved bytes.
    [[nodiscard]] virtual uint64_t reserved() const noexcept = 0;

    /// The most bytes ever charged at once.
    /// @return The peak.
    [[nodiscard]] virtual uint64_t peak() const noexcept = 0;

    /// Charges @p bytes against the budget ahead of the allocations that will draw on them.
    /// @param bytes The bytes to reserve.
    /// @return Nothing, or what the refused reservation asked for and what the budget had left.
    [[nodiscard]] virtual std::expected<void, ArenaBudgetExceeded> reserve(uint64_t bytes) noexcept = 0;

    /// Returns @p bytes of a reservation no allocation drew on.
    ///
    /// Precondition (checked): @p bytes is at most the reserved bytes not yet drawn on.
    /// @param bytes The bytes to return.
    virtual void release(uint64_t bytes) noexcept = 0;

    /// The last allocation the arena refused.
    /// @return It, or nullopt when the arena refused none.
    [[nodiscard]] virtual std::optional<ArenaRefusal> lastRefusal() const noexcept = 0;

    /// The host's memory the arena draws on: where what owns the arena itself, the prepared module's
    /// own record, is allocated, uncharged, as it cannot come from the arena it keeps alive.
    /// @return The resource, which outlives the arena.
    [[nodiscard]] virtual std::pmr::memory_resource& hostResource() const noexcept = 0;
};

/// The counting arena: it counts what it charges against a fixed budget and takes its memory from an
/// upstream resource.
class CountingArena final: public ICountingArena
{
  public:
    /// An arena of @p budget bytes over @p upstream.
    /// @param budget The bytes it may charge at once: the module's compile budget.
    /// @param upstream Where its memory comes from; the composition root injects it, and it must outlive
    ///                 the arena.
    CountingArena(uint64_t budget, std::pmr::memory_resource& upstream) noexcept;

    /// An arena of @p budget bytes over @p upstream, which it keeps alive: every container of the code it
    /// backs returns its memory there, whoever holds the code last.
    /// @param budget The bytes it may charge at once: the module's compile budget.
    /// @param upstream Where its memory comes from; the global heap when null.
    CountingArena(uint64_t budget, std::shared_ptr<std::pmr::memory_resource> upstream) noexcept;

    [[nodiscard]] uint64_t budget() const noexcept override;
    [[nodiscard]] uint64_t charged() const noexcept override;
    [[nodiscard]] uint64_t reserved() const noexcept override;
    [[nodiscard]] uint64_t peak() const noexcept override;
    [[nodiscard]] std::expected<void, ArenaBudgetExceeded> reserve(uint64_t bytes) noexcept override;
    void release(uint64_t bytes) noexcept override;
    [[nodiscard]] std::optional<ArenaRefusal> lastRefusal() const noexcept override;
    [[nodiscard]] std::pmr::memory_resource& hostResource() const noexcept override;

  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override;
    [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override;

    /// Adds @p bytes to what is charged, and to the peak if it rises past it.
    void charge(uint64_t bytes) noexcept;

    uint64_t _budget;
    std::shared_ptr<std::pmr::memory_resource>
        _ownedUpstream; ///< The upstream it keeps alive; null if borrowed.
    std::pmr::memory_resource* _upstream;
    uint64_t _charged = 0;
    uint64_t _reserved = 0; ///< The part of _charged reserved and not yet drawn on.
    uint64_t _peak = 0;
    std::optional<ArenaRefusal> _lastRefusal;
};

} // namespace isoline::runtime
