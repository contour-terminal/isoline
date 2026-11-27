// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/MemoryLayout.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <isoline/binary/Types.hpp>
#include <isoline/meta/Check.hpp>
#include <isoline/platform/Platform.hpp>

namespace isoline::runtime
{

namespace
{
    /// The access every other access of a memory relies on at least as much as: a one-byte load at
    /// offset 0. Its extent, 1, is the least, and it neither writes nor tears.
    constexpr auto LeastDemandingAccess = MemoryAccess { .offset = 0, .size = 1, .kind = AccessKind::Load };
} // namespace

StoreFaultAtomicity storeFaultAtomicityOf(platform::Architecture architecture) noexcept
{
    ISOLINE_PRECONDITION(std::to_underlying(architecture) < StoreFaultAtomicityTable.size());
    return StoreFaultAtomicityTable[std::to_underlying(architecture)].storeAtomicity;
}

StrategyRule strategyRuleFor(std::optional<MemoryLayout> const& layout,
                             binary::MemoryType const& type,
                             MemoryAccess access,
                             StoreFaultAtomicity atomicity) noexcept
{
    ISOLINE_PRECONDITION(!layout.has_value() || isWellFormed(*layout));
    ISOLINE_PRECONDITION(std::has_single_bit(access.size) && access.size <= MaximumAccessBytes);
    auto const query =
        StrategyQuery { .layout = layout, .type = type, .access = access, .atomicity = atomicity };
    auto const deciding =
        std::ranges::find_if(StrategyTable, [&](StrategyRow const& row) { return row.holds(query); });
    // The last row always holds; were it ever to decline, the access is checked.
    return deciding != StrategyTable.end() ? deciding->rule : StrategyRule::ExplicitMode;
}

BoundsStrategy boundsStrategyFor(std::optional<MemoryLayout> const& layout,
                                 binary::MemoryType const& type,
                                 MemoryAccess access,
                                 StoreFaultAtomicity atomicity) noexcept
{
    ISOLINE_PRECONDITION(!layout.has_value() || isWellFormed(*layout));
    ISOLINE_PRECONDITION(std::has_single_bit(access.size) && access.size <= MaximumAccessBytes);
    return StrategyTable[std::to_underlying(strategyRuleFor(layout, type, access, atomicity))].strategy;
}

MemoryRequirements memoryRequirementsFor(std::optional<MemoryLayout> const& layout,
                                         binary::MemoryType const& type) noexcept
{
    ISOLINE_PRECONDITION(!layout.has_value() || isWellFormed(*layout));
    // A load cannot tear, so the target's atomicity does not change its strategy.
    auto const strategy = boundsStrategyFor(layout, type, LeastDemandingAccess, StoreFaultAtomicity::MayTear);
    auto const reliance = BoundsStrategyTable[std::to_underlying(strategy)].reliance;
    // Absent only in explicit mode, whose strategy relies on nothing.
    auto const chosen = layout.value_or(MemoryLayout {});
    // Well-formed: reservation + guard is a u64.
    return MemoryRequirements {
        .guardBytes = reliance == MemoryReliance::Nothing ? 0 : chosen.guardBytes,
        .reachBytes =
            reliance == MemoryReliance::GuardAndReach ? chosen.reservationBytes + chosen.guardBytes : 0,
    };
}

MemoryLayout layoutOf(LayoutRow row, uint64_t maxMemoryBytes, uint64_t pageSize) noexcept
{
    ISOLINE_PRECONDITION(std::to_underlying(row) < MemoryLayoutTable.size());
    ISOLINE_PRECONDITION(std::has_single_bit(pageSize));
    ISOLINE_PRECONDITION(maxMemoryBytes
                         <= std::numeric_limits<uint64_t>::max() - MaximumGuardBytes - pageSize);
    auto const& published = MemoryLayoutTable[std::to_underlying(row)];
    // Rounded up to whole pages: the precondition keeps the sum, and the guard after it, a u64.
    auto const cap = (maxMemoryBytes + pageSize - 1) & ~(pageSize - 1);
    return MemoryLayout {
        .reservationBytes =
            published.reservation == ReservationSize::MemoryCap ? cap : published.reservationBytes,
        .guardBytes = published.guardBytes,
    };
}

} // namespace isoline::runtime
