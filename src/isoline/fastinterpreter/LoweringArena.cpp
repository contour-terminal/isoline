// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/LoweringArena.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <new>
#include <optional>

namespace isoline::fastinterpreter
{

CountingArena::CountingArena(uint64_t budget, std::pmr::memory_resource& upstream) noexcept:
    _budget { budget }, _upstream { &upstream }
{
}

uint64_t CountingArena::budget() const noexcept
{
    return _budget;
}

uint64_t CountingArena::charged() const noexcept
{
    return _charged;
}

uint64_t CountingArena::reserved() const noexcept
{
    return _reserved;
}

uint64_t CountingArena::peak() const noexcept
{
    return _peak;
}

std::expected<void, BudgetExceeded> CountingArena::reserve(uint64_t bytes) noexcept
{
    auto const available = _budget - _charged;
    if (bytes > available)
        return std::unexpected(BudgetExceeded { .requested = bytes, .available = available });
    charge(bytes);
    _reserved += bytes;
    return {};
}

void CountingArena::release(uint64_t bytes) noexcept
{
    Require(bytes <= _reserved);
    _reserved -= bytes;
    _charged -= bytes;
}

std::optional<Refusal> CountingArena::lastRefusal() const noexcept
{
    return _lastRefusal;
}

std::pmr::memory_resource& CountingArena::hostResource() const noexcept
{
    return *_upstream;
}

void* CountingArena::do_allocate(std::size_t bytes, std::size_t alignment)
{
    // What the reservations cover is charged already; only the rest is new to the budget.
    auto const drawn = std::min(uint64_t { bytes }, _reserved);
    auto const fresh = bytes - drawn;
    if (fresh > _budget - _charged)
    {
        _lastRefusal = Refusal { .reason = RefusalReason::Budget, .requested = bytes };
        throw std::bad_alloc();
    }
    try
    {
        auto* const pointer = _upstream->allocate(bytes, alignment);
        _reserved -= drawn;
        charge(fresh);
        return pointer;
    }
    catch (std::bad_alloc const&)
    {
        _lastRefusal = Refusal { .reason = RefusalReason::HostMemory, .requested = bytes };
        throw;
    }
}

void CountingArena::do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment)
{
    _upstream->deallocate(pointer, bytes, alignment);
    _charged -= bytes;
}

bool CountingArena::do_is_equal(std::pmr::memory_resource const& other) const noexcept
{
    return this == &other;
}

void CountingArena::charge(uint64_t bytes) noexcept
{
    _charged += bytes;
    _peak = std::max(_peak, _charged);
}

} // namespace isoline::fastinterpreter
