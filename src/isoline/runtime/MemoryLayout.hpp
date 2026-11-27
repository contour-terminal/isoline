// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include <isoline/binary/Types.hpp>
#include <isoline/meta/EnumRows.hpp>
#include <isoline/platform/Platform.hpp>

namespace isoline::runtime
{

/// The largest guard a layout may have: 2 GiB. The guard strategies fold every offset whose access ends
/// within the guard into the addressing mode, and x86-64's displacement is a signed 32-bit field, so with
/// this cap every folded offset is at most INT32_MAX.
inline constexpr uint64_t MaximumGuardBytes = uint64_t { 1 } << 31;

/// How a guard-page engine lays out every linear memory: the address space reserved from its first byte,
/// and the inaccessible guard after that. Fixed at the engine's construction; explicit mode has none
/// (std::nullopt wherever a layout is taken).
struct MemoryLayout
{
    uint64_t reservationBytes = 0; ///< The bytes reserved from the memory's base.
    uint64_t guardBytes = 0;       ///< The inaccessible bytes after the reservation.

    /// Whether both lay memories out alike.
    [[nodiscard]] constexpr bool operator==(MemoryLayout const&) const noexcept = default;
};

/// Whether boundsStrategyFor() and memoryRequirementsFor() accept @p layout: its guard is at most
/// MaximumGuardBytes, and its reservation plus its guard is a u64.
/// @param layout The layout.
/// @return Whether it is well-formed.
[[nodiscard]] constexpr bool isWellFormed(MemoryLayout const& layout) noexcept
{
    return layout.guardBytes <= MaximumGuardBytes
           && layout.reservationBytes <= std::numeric_limits<uint64_t>::max() - layout.guardBytes;
}

/// How one access into a linear memory is bounds-checked.
enum class BoundsStrategy : uint8_t
{
    /// The index, offset and size are checked against the memory's size before the access: no fault is
    /// relied on.
    Explicit,
    /// The index is checked against the memory's size; the offset is folded into the address, and an
    /// access past the end faults in the guard. Under the Spectre guard the whole effective address is
    /// clamped, never only the index.
    IndexOnly,
    /// Nothing is checked: every index and folded offset lands inside the reservation and its guard, and
    /// faults there unless it is in bounds.
    Elided,
};

/// The last BoundsStrategy enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastBoundsStrategy = BoundsStrategy::Elided;

/// What a strategy relies on the memory to provide.
enum class MemoryReliance : uint8_t
{
    Nothing,       ///< Nothing: every access is checked.
    Guard,         ///< ILinearMemory::guardBytes() of at least the layout's guard, after every grow.
    GuardAndReach, ///< The guard, and bytes().size() + guardBytes() of at least reservation + guard.
};

/// The last MemoryReliance enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastMemoryReliance = MemoryReliance::GuardAndReach;

/// What one BoundsStrategy relies on: one row of BoundsStrategyTable.
struct BoundsStrategyRow
{
    BoundsStrategy strategy = BoundsStrategy::Explicit; ///< The strategy.
    MemoryReliance reliance = MemoryReliance::Nothing;  ///< What it relies on the memory for.
};

/// Every BoundsStrategy, at its enumerator's index: the Decisions' "relies on" column.
inline constexpr auto BoundsStrategyTable = std::to_array<BoundsStrategyRow>({
    { .strategy = BoundsStrategy::Explicit, .reliance = MemoryReliance::Nothing },
    { .strategy = BoundsStrategy::IndexOnly, .reliance = MemoryReliance::Guard },
    { .strategy = BoundsStrategy::Elided, .reliance = MemoryReliance::GuardAndReach },
});

static_assert(meta::firstMisplacedRow<LastBoundsStrategy>(BoundsStrategyTable, &BoundsStrategyRow::strategy)
                  == meta::NoMisplacedRow,
              "BoundsStrategyTable has one row per enumerator, at its index");

/// Whether an access reads or writes.
enum class AccessKind : uint8_t
{
    Load,  ///< It reads.
    Store, ///< It writes.
};

/// The last AccessKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastAccessKind = AccessKind::Store;

/// What a target's store does when its bytes straddle the end of the accessible memory: a property of the
/// architecture, which decides whether the guard may catch a store wider than a byte.
enum class StoreFaultAtomicity : uint8_t
{
    /// The store faults before it writes any byte (x86-64): the guard catches it like a load.
    AllOrNothing,
    /// The store may write its bytes below the end before it faults (arm64: not architecturally
    /// guaranteed), so a store wider than a byte must be checked explicitly.
    MayTear,
};

/// The last StoreFaultAtomicity enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastStoreFaultAtomicity = StoreFaultAtomicity::MayTear;

/// What one architecture's straddling store does: one row of StoreFaultAtomicityTable.
struct StoreFaultAtomicityRow
{
    platform::Architecture architecture = platform::Architecture::X64; ///< The architecture.
    StoreFaultAtomicity storeAtomicity = StoreFaultAtomicity::MayTear; ///< What its stores do.
};

/// Every platform::Architecture, at its enumerator's index. An architecture without a backend is taken as
/// MayTear, the weaker promise. The compiler's TargetFaultRow (P7 Task 10) takes its column from here.
inline constexpr auto StoreFaultAtomicityTable = std::to_array<StoreFaultAtomicityRow>({
    { .architecture = platform::Architecture::X64, .storeAtomicity = StoreFaultAtomicity::AllOrNothing },
    { .architecture = platform::Architecture::Arm64, .storeAtomicity = StoreFaultAtomicity::MayTear },
    { .architecture = platform::Architecture::Other, .storeAtomicity = StoreFaultAtomicity::MayTear },
});

static_assert(meta::firstMisplacedRow<platform::LastArchitecture>(StoreFaultAtomicityTable,
                                                                  &StoreFaultAtomicityRow::architecture)
                  == meta::NoMisplacedRow,
              "StoreFaultAtomicityTable has one row per enumerator, at its index");

/// What @p architecture's straddling stores do.
///
/// Precondition (checked): @p architecture is an enumerator.
/// @param architecture The architecture.
/// @return Its row's StoreFaultAtomicity.
[[nodiscard]] StoreFaultAtomicity storeFaultAtomicityOf(platform::Architecture architecture) noexcept;

/// The widest access Wasm has, a v128: every MemoryAccess::size is a power of two up to it.
inline constexpr uint8_t MaximumAccessBytes = 16;

/// One access into a linear memory, as the compiler sees it: a static offset and a size, at a dynamic
/// index.
struct MemoryAccess
{
    uint64_t offset = 0;                ///< The instruction's static offset.
    uint8_t size = 0;                   ///< The bytes it reads or writes: 1, 2, 4, 8 or 16.
    AccessKind kind = AccessKind::Load; ///< Whether it reads or writes.
};

/// Why an access takes its strategy: one row of StrategyTable each, asked in this order.
enum class StrategyRule : uint8_t
{
    ExplicitMode,    ///< No layout: the embedder chose explicit checks.
    Memory64,        ///< 64-bit indices, which no guard covers (spec §7).
    CustomPageSize,  ///< A page size other than 64 KiB (spec §7).
    BeyondTheGuard,  ///< offset + size exceeds the guard, so the guard cannot catch the access.
    TearingStore,    ///< A store wider than a byte, on a target whose straddling stores may tear.
    WholeIndexRange, ///< The reservation holds every i32 index and the memory's maximum: no check at all.
    FoldedOffset,    ///< Otherwise: the index is checked, and the offset folded.
};

/// The last StrategyRule enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastStrategyRule = StrategyRule::FoldedOffset;

/// One access as StrategyTable's predicates ask about it. It refers to the layout and the type it was built
/// from, without owning them: build it from objects that outlive it (strategyRuleFor() builds one on its
/// stack per call), and never keep it past the call that asks.
struct StrategyQuery
{
    std::optional<MemoryLayout> const& layout; ///< The engine's layout; std::nullopt in explicit mode.
    binary::MemoryType const& type;            ///< The memory's type.
    MemoryAccess access {};                    ///< The access.
    /// What the target's straddling stores do.
    StoreFaultAtomicity atomicity = StoreFaultAtomicity::MayTear;
};

namespace detail
{
    /// The bytes of a 64 KiB page, the only page size the guard strategies apply to.
    inline constexpr auto DefaultPageBytes = uint64_t { 1 } << binary::DefaultPageSizeLog2;

    /// The bytes every i32 index together spans: 2^32.
    inline constexpr auto Index32Range = uint64_t { 1 } << 32;

    /// The guard of @p query's layout; 0 in explicit mode.
    [[nodiscard]] constexpr uint64_t guardOf(StrategyQuery const& query) noexcept
    {
        return query.layout.has_value() ? query.layout->guardBytes : 0;
    }

    /// The reservation of @p query's layout; 0 in explicit mode.
    [[nodiscard]] constexpr uint64_t reservationOf(StrategyQuery const& query) noexcept
    {
        return query.layout.has_value() ? query.layout->reservationBytes : 0;
    }

    /// StrategyRule::ExplicitMode: whether there is no layout.
    [[nodiscard]] constexpr bool isExplicitMode(StrategyQuery const& query) noexcept
    {
        return !query.layout.has_value();
    }

    /// StrategyRule::Memory64: whether the memory's addresses are i64.
    [[nodiscard]] constexpr bool isMemory64(StrategyQuery const& query) noexcept
    {
        return query.type.limits.addressType == binary::AddressType::I64;
    }

    /// StrategyRule::CustomPageSize: whether the memory's pages are not 64 KiB.
    [[nodiscard]] constexpr bool hasCustomPageSize(StrategyQuery const& query) noexcept
    {
        return query.type.pageSizeLog2 != binary::DefaultPageSizeLog2;
    }

    /// StrategyRule::BeyondTheGuard: whether offset + size exceeds the guard, decided without the sum, so
    /// that a forged offset near 2^64 cannot wrap below it.
    [[nodiscard]] constexpr bool endsBeyondTheGuard(StrategyQuery const& query) noexcept
    {
        auto const guard = guardOf(query);
        return query.access.offset > guard || query.access.size > guard - query.access.offset;
    }

    /// StrategyRule::TearingStore: whether the access is a store wider than a byte on a target whose
    /// straddling stores may tear.
    [[nodiscard]] constexpr bool isTearingStore(StrategyQuery const& query) noexcept
    {
        return query.access.kind == AccessKind::Store && query.access.size > 1
               && query.atomicity == StoreFaultAtomicity::MayTear;
    }

    /// StrategyRule::WholeIndexRange: whether every i32 index, plus an extent within the guard, stays below
    /// reservation + guard, and the memory never grows past the reservation. Its maximum is compared in
    /// pages, so that no forged maximum overflows a byte count.
    [[nodiscard]] constexpr bool coversTheWholeIndexRange(StrategyQuery const& query) noexcept
    {
        auto const reservation = reservationOf(query);
        auto const maximumPages = query.type.limits.max.value_or(binary::maxPages(query.type));
        return query.type.limits.addressType == binary::AddressType::I32
               && query.type.pageSizeLog2 == binary::DefaultPageSizeLog2 && reservation >= Index32Range
               && maximumPages <= reservation / DefaultPageBytes;
    }

    /// StrategyRule::FoldedOffset: always, the decision's last row.
    [[nodiscard]] constexpr bool otherwise(StrategyQuery const& /*query*/) noexcept
    {
        return true;
    }
} // namespace detail

/// One rule of the strategy decision: when it holds, and what the access then takes.
struct StrategyRow
{
    StrategyRule rule = StrategyRule::ExplicitMode;     ///< The rule.
    BoundsStrategy strategy = BoundsStrategy::Explicit; ///< What an access it decides takes.
    /// Whether it decides a query. Asked only once every row above declined; each still reads only what
    /// is there, so that no order of asking can dereference an absent layout.
    bool (*holds)(StrategyQuery const& query) noexcept = nullptr;
};

/// The strategy decision, as ordered rules: strategyRuleFor() takes the first row that holds, and the last
/// always does. Every row before the guard strategies' names a case those cannot cover.
inline constexpr auto StrategyTable = std::to_array<StrategyRow>({
    { .rule = StrategyRule::ExplicitMode,
      .strategy = BoundsStrategy::Explicit,
      .holds = &detail::isExplicitMode },
    { .rule = StrategyRule::Memory64, .strategy = BoundsStrategy::Explicit, .holds = &detail::isMemory64 },
    { .rule = StrategyRule::CustomPageSize,
      .strategy = BoundsStrategy::Explicit,
      .holds = &detail::hasCustomPageSize },
    { .rule = StrategyRule::BeyondTheGuard,
      .strategy = BoundsStrategy::Explicit,
      .holds = &detail::endsBeyondTheGuard },
    { .rule = StrategyRule::TearingStore,
      .strategy = BoundsStrategy::Explicit,
      .holds = &detail::isTearingStore },
    { .rule = StrategyRule::WholeIndexRange,
      .strategy = BoundsStrategy::Elided,
      .holds = &detail::coversTheWholeIndexRange },
    { .rule = StrategyRule::FoldedOffset,
      .strategy = BoundsStrategy::IndexOnly,
      .holds = &detail::otherwise },
});

static_assert(std::ranges::all_of(StrategyTable, [](StrategyRow const& row) { return row.holds != nullptr; }),
              "every StrategyTable row has a predicate");

static_assert(meta::firstMisplacedRow<LastStrategyRule>(StrategyTable, &StrategyRow::rule)
                  == meta::NoMisplacedRow,
              "StrategyTable has one row per enumerator, at its index");

/// The rule that decides one access into a memory of @p type, under @p layout, on a target whose
/// straddling stores behave as @p atomicity says: the first row of StrategyTable that holds.
///
/// Preconditions (checked): @p layout is std::nullopt or well-formed (isWellFormed()); @p access's size is a
/// power of two of at most MaximumAccessBytes.
/// @param layout The engine's layout; std::nullopt in explicit mode.
/// @param type The memory's type.
/// @param access The access.
/// @param atomicity What the target's straddling stores do.
/// @return The deciding rule.
[[nodiscard]] StrategyRule strategyRuleFor(std::optional<MemoryLayout> const& layout,
                                           binary::MemoryType const& type,
                                           MemoryAccess access,
                                           StoreFaultAtomicity atomicity) noexcept;

/// The strategy of one access into a memory of @p type, under @p layout (std::nullopt: explicit mode), on a
/// target with @p atomicity: the strategy of strategyRuleFor()'s row. The compiler asks it per access, and
/// memoryRequirementsFor() derives the store's check from it, so the two agree by construction.
///
/// Preconditions (checked): @p layout is std::nullopt or well-formed (isWellFormed()); @p access's size is a
/// power of two of at most MaximumAccessBytes.
/// @param layout The engine's layout; std::nullopt in explicit mode.
/// @param type The memory's type.
/// @param access The access.
/// @param atomicity What the target's straddling stores do.
/// @return The strategy.
[[nodiscard]] BoundsStrategy boundsStrategyFor(std::optional<MemoryLayout> const& layout,
                                               binary::MemoryType const& type,
                                               MemoryAccess access,
                                               StoreFaultAtomicity atomicity) noexcept;

/// What one memory must provide for the code that accesses it: what the store checks once, at allocation.
struct MemoryRequirements
{
    uint64_t guardBytes = 0; ///< The least ILinearMemory::guardBytes(); 0 requires none.
    /// The least bytes().size() + guardBytes(): how far from the memory's base every address must stay
    /// inaccessible unless it is in bounds. 0 requires none.
    uint64_t reachBytes = 0;

    /// Whether both require the same.
    [[nodiscard]] constexpr bool operator==(MemoryRequirements const&) const noexcept = default;
};

/// What a memory of @p type must provide under @p layout: what the least demanding access, a one-byte
/// load at offset 0, relies on (BoundsStrategyTable). No access of the memory takes a strategy that relies
/// on more, since every rule above the guard strategies holds for it whenever it holds for that load.
///
/// {0, 0} in explicit mode, for memory64 and for custom page sizes; {guard, 0} where the offset is folded;
/// {guard, reservation + guard} where checks are elided.
///
/// Precondition (checked): @p layout is std::nullopt or well-formed (isWellFormed()).
/// @param layout The engine's layout; std::nullopt in explicit mode.
/// @param type The memory's type.
/// @return Its requirements.
[[nodiscard]] MemoryRequirements memoryRequirementsFor(std::optional<MemoryLayout> const& layout,
                                                       binary::MemoryType const& type) noexcept;

/// A published layout (MemoryLayoutTable): data an embedder picks, never a default.
enum class LayoutRow : uint8_t
{
    WasmtimeParity, ///< Wasmtime's defaults, for 64-bit Linux, macOS and Windows: 4 GiB, guard 32 MiB.
    /// The memory cap as the reservation, guard 32 MiB: FreeBSD, whose RLIMIT_AS cannot hold a 4 GiB
    /// reservation, and any embedder under an address-space cap.
    Capped,
};

/// The last LayoutRow enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastLayoutRow = LayoutRow::Capped;

/// Where a published layout's reservation comes from.
enum class ReservationSize : uint8_t
{
    Fixed,     ///< The row's reservationBytes.
    MemoryCap, ///< The embedder's memory cap (StoreLimits::maxMemoryBytes), rounded up to whole pages.
};

/// The last ReservationSize enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastReservationSize = ReservationSize::MemoryCap;

/// One published layout: a row of MemoryLayoutTable.
struct MemoryLayoutRow
{
    LayoutRow row = LayoutRow::WasmtimeParity;            ///< The row.
    std::string_view name;                                ///< Its name, as configuration spells it.
    ReservationSize reservation = ReservationSize::Fixed; ///< Where its reservation comes from.
    uint64_t reservationBytes = 0;                        ///< Its reservation when Fixed; 0 otherwise.
    uint64_t guardBytes = 0;                              ///< Its guard.
};

/// Every LayoutRow, at its enumerator's index.
inline constexpr auto MemoryLayoutTable = std::to_array<MemoryLayoutRow>({
    { .row = LayoutRow::WasmtimeParity,
      .name = "wasmtime-parity",
      .reservation = ReservationSize::Fixed,
      .reservationBytes = uint64_t { 4 } << 30,
      .guardBytes = uint64_t { 32 } << 20 },
    { .row = LayoutRow::Capped,
      .name = "capped",
      .reservation = ReservationSize::MemoryCap,
      .reservationBytes = 0,
      .guardBytes = uint64_t { 32 } << 20 },
});

static_assert(meta::firstMisplacedRow<LastLayoutRow>(MemoryLayoutTable, &MemoryLayoutRow::row)
                  == meta::NoMisplacedRow,
              "MemoryLayoutTable has one row per enumerator, at its index");

/// The layout of published row @p row, for an embedder whose memories may hold @p maxMemoryBytes and whose
/// pages are @p pageSize bytes. Always well-formed (isWellFormed()).
///
/// Preconditions (checked): @p row is an enumerator; @p pageSize is a power of two; @p maxMemoryBytes is at
/// most 2^64 - 1 - MaximumGuardBytes - @p pageSize, so that its rounding and the guard after it are a u64.
/// @param row The published row.
/// @param maxMemoryBytes The memory cap, which a MemoryCap row reserves.
/// @param pageSize The system's page size (platform::IVirtualMemory::pageSize()).
/// @return The layout.
[[nodiscard]] MemoryLayout layoutOf(LayoutRow row, uint64_t maxMemoryBytes, uint64_t pageSize) noexcept;

} // namespace isoline::runtime
