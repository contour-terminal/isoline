// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <isoline/binary/Types.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/runtime/MemoryLayout.hpp>

using isoline::binary::AddressType;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::platform::Architecture;
using isoline::runtime::AccessKind;
using isoline::runtime::BoundsStrategy;
using isoline::runtime::boundsStrategyFor;
using isoline::runtime::BoundsStrategyTable;
using isoline::runtime::isWellFormed;
using isoline::runtime::layoutOf;
using isoline::runtime::LayoutRow;
using isoline::runtime::MaximumGuardBytes;
using isoline::runtime::MemoryAccess;
using isoline::runtime::MemoryLayout;
using isoline::runtime::MemoryLayoutTable;
using isoline::runtime::MemoryReliance;
using isoline::runtime::MemoryRequirements;
using isoline::runtime::memoryRequirementsFor;
using isoline::runtime::ReservationSize;
using isoline::runtime::StoreFaultAtomicity;
using isoline::runtime::storeFaultAtomicityOf;
using isoline::runtime::StoreFaultAtomicityTable;
using isoline::runtime::StrategyRule;
using isoline::runtime::strategyRuleFor;
using isoline::runtime::StrategyTable;

namespace
{

constexpr auto KiB = uint64_t { 1 } << 10;
constexpr auto MiB = uint64_t { 1 } << 20;
constexpr auto GiB = uint64_t { 1 } << 30;

/// The Wasmtime-parity layout: a 4 GiB reservation and a 32 MiB guard.
constexpr auto Parity = MemoryLayout { .reservationBytes = 4 * GiB, .guardBytes = 32 * MiB };

/// Contour's layout: a reservation of the 32 MiB memory cap and a 32 MiB guard.
constexpr auto Capped = MemoryLayout { .reservationBytes = 32 * MiB, .guardBytes = 32 * MiB };

/// A memory of i32 addresses and 64 KiB pages, from @p min to @p max pages.
MemoryType memory32(uint64_t min, std::optional<uint64_t> max)
{
    return MemoryType { .limits = Limits { .min = min, .max = max, .addressType = AddressType::I32 } };
}

/// A memory of i64 addresses and 64 KiB pages, from @p min to @p max pages.
MemoryType memory64(uint64_t min, std::optional<uint64_t> max)
{
    return MemoryType { .limits = Limits { .min = min, .max = max, .addressType = AddressType::I64 } };
}

/// A memory of i32 addresses and pages of 2^@p pageSizeLog2 bytes, from @p min to @p max pages.
MemoryType customPages(uint8_t pageSizeLog2, uint64_t min, uint64_t max)
{
    return MemoryType { .limits = Limits { .min = min, .max = max, .addressType = AddressType::I32 },
                        .pageSizeLog2 = pageSizeLog2 };
}

/// A load of @p size bytes at @p offset.
constexpr MemoryAccess load(uint64_t offset, uint8_t size)
{
    return MemoryAccess { .offset = offset, .size = size, .kind = AccessKind::Load };
}

/// A store of @p size bytes at @p offset.
constexpr MemoryAccess store(uint64_t offset, uint8_t size)
{
    return MemoryAccess { .offset = offset, .size = size, .kind = AccessKind::Store };
}

/// The strategy of @p access into @p type under @p layout on an AllOrNothing target.
BoundsStrategy strategyOf(std::optional<MemoryLayout> const& layout,
                          MemoryType const& type,
                          MemoryAccess access)
{
    return boundsStrategyFor(layout, type, access, StoreFaultAtomicity::AllOrNothing);
}

/// The sizes a Wasm access has: a byte up to a v128.
constexpr auto AccessSizes = std::to_array<uint8_t>({ 1, 2, 4, 8, 16 });

} // namespace

TEST_CASE("MemoryLayout.strategyTable")
{
    // One case per row and per boundary.
    CHECK(strategyOf(Parity, memory32(1, 65'536), load(0, 4)) == BoundsStrategy::Elided);
    CHECK(strategyOf(Parity, memory32(1, 65'536), load(32 * MiB, 1)) == BoundsStrategy::Explicit);
    CHECK(strategyOf(Parity, memory32(1, 65'536), load((32 * MiB) - 4, 4)) == BoundsStrategy::Elided);
    CHECK(strategyOf(Parity, memory32(1, 65'536), load((32 * MiB) - 3, 4)) == BoundsStrategy::Explicit);
    CHECK(strategyOf(Capped, memory32(1, 512), load(4, 4)) == BoundsStrategy::IndexOnly);
    CHECK(strategyOf(std::nullopt, memory32(1, 512), load(4, 4)) == BoundsStrategy::Explicit);

    // Every row decides some access, and gives it the row's strategy.
    struct Case
    {
        StrategyRule rule = StrategyRule::ExplicitMode;
        std::optional<MemoryLayout> layout;
        MemoryType type;
        MemoryAccess access;
        StoreFaultAtomicity atomicity = StoreFaultAtomicity::AllOrNothing;
    };
    auto const cases = std::to_array<Case>({
        { StrategyRule::ExplicitMode,
          std::nullopt,
          memory32(1, 1),
          load(0, 1),
          StoreFaultAtomicity::AllOrNothing },
        { StrategyRule::Memory64, Parity, memory64(1, 1), load(0, 1), StoreFaultAtomicity::AllOrNothing },
        { StrategyRule::CustomPageSize,
          Parity,
          customPages(0, 1, 1),
          load(0, 1),
          StoreFaultAtomicity::AllOrNothing },
        { StrategyRule::BeyondTheGuard,
          Parity,
          memory32(1, 1),
          load(32 * MiB, 1),
          StoreFaultAtomicity::AllOrNothing },
        { StrategyRule::TearingStore, Parity, memory32(1, 1), store(0, 2), StoreFaultAtomicity::MayTear },
        { StrategyRule::WholeIndexRange,
          Parity,
          memory32(1, 1),
          load(0, 1),
          StoreFaultAtomicity::AllOrNothing },
        { StrategyRule::FoldedOffset, Capped, memory32(1, 1), load(0, 1), StoreFaultAtomicity::AllOrNothing },
    });
    REQUIRE(cases.size() == StrategyTable.size());
    for (auto const [index, entry]: core::views::enumerate(cases))
    {
        CAPTURE(index);
        CHECK(entry.rule == StrategyTable[index].rule);
        CHECK(strategyRuleFor(entry.layout, entry.type, entry.access, entry.atomicity) == entry.rule);
        CHECK(boundsStrategyFor(entry.layout, entry.type, entry.access, entry.atomicity)
              == StrategyTable[index].strategy);
    }

    // The table's strategies are the Decisions' rows: only the last two rows relax a check.
    CHECK(StrategyTable[std::to_underlying(StrategyRule::WholeIndexRange)].strategy
          == BoundsStrategy::Elided);
    CHECK(StrategyTable[std::to_underlying(StrategyRule::FoldedOffset)].strategy
          == BoundsStrategy::IndexOnly);
    CHECK(BoundsStrategyTable[std::to_underlying(BoundsStrategy::Explicit)].reliance
          == MemoryReliance::Nothing);
    CHECK(BoundsStrategyTable[std::to_underlying(BoundsStrategy::IndexOnly)].reliance
          == MemoryReliance::Guard);
    CHECK(BoundsStrategyTable[std::to_underlying(BoundsStrategy::Elided)].reliance
          == MemoryReliance::GuardAndReach);
}

TEST_CASE("MemoryLayout.tearingStoresStayExplicit")
{
    auto const memory = memory32(1, 512);
    // On a target whose straddling stores may write part of their bytes, a store wider than a byte
    // must not rely on the guard.
    for (auto const size: AccessSizes)
    {
        CAPTURE(size);
        auto const expected = size == 1 ? BoundsStrategy::IndexOnly : BoundsStrategy::Explicit;
        CHECK(boundsStrategyFor(Capped, memory, store(4, size), StoreFaultAtomicity::MayTear) == expected);
        // Loads write nothing, so they keep the guard strategy.
        CHECK(boundsStrategyFor(Capped, memory, load(4, size), StoreFaultAtomicity::MayTear)
              == BoundsStrategy::IndexOnly);
        // A target that faults before writing any byte keeps it for stores too.
        CHECK(boundsStrategyFor(Capped, memory, store(4, size), StoreFaultAtomicity::AllOrNothing)
              == BoundsStrategy::IndexOnly);
    }
    // The same under the parity layout, whose guard strategy is Elided.
    CHECK(boundsStrategyFor(Parity, memory, store(0, 2), StoreFaultAtomicity::MayTear)
          == BoundsStrategy::Explicit);
    CHECK(boundsStrategyFor(Parity, memory, store(0, 1), StoreFaultAtomicity::MayTear)
          == BoundsStrategy::Elided);
    CHECK(boundsStrategyFor(Parity, memory, load(0, 8), StoreFaultAtomicity::MayTear)
          == BoundsStrategy::Elided);
}

TEST_CASE("MemoryLayout.memory64AndCustomPagesStayExplicit")
{
    for (auto const& layout: { Parity, Capped })
    {
        for (auto const size: AccessSizes)
        {
            CAPTURE(layout.reservationBytes, size);
            CHECK(strategyOf(layout, memory64(1, std::nullopt), load(0, size)) == BoundsStrategy::Explicit);
            CHECK(strategyOf(layout, memory64(1, 1), store(0, size)) == BoundsStrategy::Explicit);
            CHECK(strategyOf(layout, customPages(0, 1, 1), load(0, size)) == BoundsStrategy::Explicit);
        }
        CHECK(memoryRequirementsFor(layout, memory64(1, std::nullopt)) == MemoryRequirements {});
        CHECK(memoryRequirementsFor(layout, customPages(0, 1, 1)) == MemoryRequirements {});
    }
    // A page size the validator refuses, but the decoder admits, is explicit too, and shifts nothing.
    CHECK(strategyOf(Parity, customPages(63, 0, 0), load(0, 1)) == BoundsStrategy::Explicit);
    CHECK(strategyOf(Parity, customPages(15, 1, 1), load(0, 1)) == BoundsStrategy::Explicit);
    CHECK(strategyOf(Parity, customPages(17, 1, 1), load(0, 1)) == BoundsStrategy::Explicit);
    CHECK(memoryRequirementsFor(Parity, customPages(63, 0, 0)) == MemoryRequirements {});
}

TEST_CASE("MemoryLayout.elisionNeedsTheWholeIndexRange")
{
    // A reservation one page short of 4 GiB does not cover every i32 index.
    auto const short4GiB =
        MemoryLayout { .reservationBytes = (4 * GiB) - (64 * KiB), .guardBytes = 32 * MiB };
    CHECK(strategyOf(short4GiB, memory32(1, std::nullopt), load(0, 4)) == BoundsStrategy::IndexOnly);
    CHECK(memoryRequirementsFor(short4GiB, memory32(1, std::nullopt))
          == MemoryRequirements { .guardBytes = 32 * MiB, .reachBytes = 0 });

    // Exactly 4 GiB does, whether the memory declares a maximum or not.
    CHECK(strategyOf(Parity, memory32(1, std::nullopt), load(0, 4)) == BoundsStrategy::Elided);
    CHECK(strategyOf(Parity, memory32(0, 0), load(0, 4)) == BoundsStrategy::Elided);

    // A maximum past the reservation (a type the validator refuses: 2^16 + 1 pages of i32 addresses)
    // keeps the index check.
    CHECK(strategyOf(Parity, memory32(1, 65'537), load(0, 4)) == BoundsStrategy::IndexOnly);
    CHECK(strategyOf(Parity, memory32(1, std::numeric_limits<uint64_t>::max()), load(0, 4))
          == BoundsStrategy::IndexOnly);
    CHECK(memoryRequirementsFor(Parity, memory32(1, 65'537))
          == MemoryRequirements { .guardBytes = 32 * MiB, .reachBytes = 0 });
    // A reservation that holds it elides again.
    auto const eightGiB = MemoryLayout { .reservationBytes = 8 * GiB, .guardBytes = 32 * MiB };
    CHECK(strategyOf(eightGiB, memory32(1, 65'537), load(0, 4)) == BoundsStrategy::Elided);
    CHECK(memoryRequirementsFor(eightGiB, memory32(1, 65'537))
          == MemoryRequirements { .guardBytes = 32 * MiB, .reachBytes = (8 * GiB) + (32 * MiB) });
}

TEST_CASE("MemoryLayout.guardAboveTwoGiBIsRefused")
{
    // What both functions accept: a guard of at most 2 GiB, and a reach that is a u64. Each function's
    // precondition is this, which the canary's memory-layout-* cases prove ends the process.
    CHECK(isWellFormed(MemoryLayout { .reservationBytes = 0, .guardBytes = 2 * GiB }));
    CHECK_FALSE(isWellFormed(MemoryLayout { .reservationBytes = 0, .guardBytes = (2 * GiB) + (64 * KiB) }));
    CHECK_FALSE(isWellFormed(MemoryLayout { .reservationBytes = 0, .guardBytes = MaximumGuardBytes + 1 }));
    auto const largest = std::numeric_limits<uint64_t>::max();
    CHECK(isWellFormed(MemoryLayout { .reservationBytes = largest - (2 * GiB), .guardBytes = 2 * GiB }));
    CHECK_FALSE(
        isWellFormed(MemoryLayout { .reservationBytes = largest - (2 * GiB) + 1, .guardBytes = 2 * GiB }));
    CHECK_FALSE(isWellFormed(MemoryLayout { .reservationBytes = largest, .guardBytes = 1 }));

    // Under the largest guard every offset up to 2^31 − size folds, and none past it.
    auto const twoGiB = MemoryLayout { .reservationBytes = 32 * MiB, .guardBytes = 2 * GiB };
    for (auto const size: AccessSizes)
    {
        CAPTURE(size);
        auto const largestFolded = (2 * GiB) - size;
        CHECK(largestFolded <= uint64_t { std::numeric_limits<int32_t>::max() });
        CHECK(strategyOf(twoGiB, memory32(1, 1), load(largestFolded, size)) == BoundsStrategy::IndexOnly);
        CHECK(strategyOf(twoGiB, memory32(1, 1), load(largestFolded + 1, size)) == BoundsStrategy::Explicit);
        // An offset whose extent wraps a u64 (offset + size == 2^64) is past the guard, never folded.
        CHECK(strategyOf(twoGiB, memory32(1, 1), load(largest - size + 1, size)) == BoundsStrategy::Explicit);
        CHECK(strategyOf(twoGiB, memory32(1, 1), load(largest, size)) == BoundsStrategy::Explicit);
    }
    // A guard of 0 folds nothing, and so requires nothing.
    auto const noGuard = MemoryLayout { .reservationBytes = 4 * GiB, .guardBytes = 0 };
    CHECK(strategyOf(noGuard, memory32(1, 1), load(0, 1)) == BoundsStrategy::Explicit);
    CHECK(memoryRequirementsFor(noGuard, memory32(1, 1)) == MemoryRequirements {});
}

TEST_CASE("MemoryLayout.requirementsPerMemoryType")
{
    CHECK(memoryRequirementsFor(Parity, memory32(1, 65'536))
          == MemoryRequirements { .guardBytes = 32 * MiB, .reachBytes = (4 * GiB) + (32 * MiB) });
    CHECK(memoryRequirementsFor(Capped, memory32(1, 512))
          == MemoryRequirements { .guardBytes = 32 * MiB, .reachBytes = 0 });
    CHECK(memoryRequirementsFor(Parity, memory64(1, 1)) == MemoryRequirements {});
    CHECK(memoryRequirementsFor(std::nullopt, memory32(1, 512)) == MemoryRequirements {});

    // The largest reach a well-formed layout has is a u64.
    auto const largest = MemoryLayout { .reservationBytes = std::numeric_limits<uint64_t>::max() - (2 * GiB),
                                        .guardBytes = 2 * GiB };
    CHECK(
        memoryRequirementsFor(largest, memory32(0, std::nullopt))
        == MemoryRequirements { .guardBytes = 2 * GiB, .reachBytes = std::numeric_limits<uint64_t>::max() });
}

TEST_CASE("MemoryLayout.requirementsCoverEveryAccess")
{
    // The store checks a memory once, against memoryRequirementsFor(); the compiler asks
    // boundsStrategyFor() per access. Whatever any access relies on, the requirements hold.
    auto const layouts = std::to_array<std::optional<MemoryLayout>>({
        std::nullopt,
        Parity,
        Capped,
        MemoryLayout { .reservationBytes = (4 * GiB) - (64 * KiB), .guardBytes = 32 * MiB },
        MemoryLayout { .reservationBytes = 8 * GiB, .guardBytes = 2 * GiB },
        MemoryLayout { .reservationBytes = 4 * GiB, .guardBytes = 0 },
    });
    auto const types = std::to_array<MemoryType>({
        memory32(0, std::nullopt),
        memory32(1, 512),
        memory32(1, 65'537),
        memory64(1, std::nullopt),
        customPages(0, 1, 1),
    });
    auto const offsets = std::to_array<uint64_t>({ 0, 4'096, (32 * MiB) - 16, 32 * MiB, 2 * GiB, 4 * GiB });
    for (auto const& layout: layouts)
        for (auto const& type: types)
        {
            auto const requirements = memoryRequirementsFor(layout, type);
            for (auto const offset: offsets)
                for (auto const size: AccessSizes)
                    for (auto const kind: { AccessKind::Load, AccessKind::Store })
                        for (auto const atomicity:
                             { StoreFaultAtomicity::AllOrNothing, StoreFaultAtomicity::MayTear })
                        {
                            auto const access = MemoryAccess { .offset = offset, .size = size, .kind = kind };
                            auto const strategy = boundsStrategyFor(layout, type, access, atomicity);
                            auto const reliance = BoundsStrategyTable[std::to_underlying(strategy)].reliance;
                            CAPTURE(offset, size, std::to_underlying(strategy));
                            if (reliance != MemoryReliance::Nothing)
                            {
                                REQUIRE(layout.has_value());
                                CHECK(requirements.guardBytes >= layout->guardBytes);
                            }
                            if (reliance == MemoryReliance::GuardAndReach)
                                CHECK(requirements.reachBytes
                                      >= layout->reservationBytes + layout->guardBytes);
                        }
        }
}

TEST_CASE("MemoryLayout.publishedRows")
{
    CHECK(MemoryLayoutTable[std::to_underlying(LayoutRow::WasmtimeParity)].name == "wasmtime-parity");
    CHECK(MemoryLayoutTable[std::to_underlying(LayoutRow::Capped)].name == "capped");
    CHECK(MemoryLayoutTable[std::to_underlying(LayoutRow::WasmtimeParity)].reservation
          == ReservationSize::Fixed);
    CHECK(MemoryLayoutTable[std::to_underlying(LayoutRow::Capped)].reservation == ReservationSize::MemoryCap);

    // Parity ignores the cap; Capped reserves it, rounded up to whole pages.
    CHECK(layoutOf(LayoutRow::WasmtimeParity, 32 * MiB, 4 * KiB) == Parity);
    CHECK(layoutOf(LayoutRow::WasmtimeParity, 0, 64 * KiB) == Parity);
    CHECK(layoutOf(LayoutRow::Capped, 32 * MiB, 4 * KiB) == Capped);
    CHECK(layoutOf(LayoutRow::Capped, MiB + 1, 4 * KiB)
          == MemoryLayout { .reservationBytes = MiB + (4 * KiB), .guardBytes = 32 * MiB });
    CHECK(layoutOf(LayoutRow::Capped, MiB + 1, 64 * KiB)
          == MemoryLayout { .reservationBytes = MiB + (64 * KiB), .guardBytes = 32 * MiB });
    CHECK(layoutOf(LayoutRow::Capped, 0, 4 * KiB)
          == MemoryLayout { .reservationBytes = 0, .guardBytes = 32 * MiB });

    // Every row's layout is one both functions accept, even at the largest cap layoutOf() takes.
    auto const largestCap = std::numeric_limits<uint64_t>::max() - MaximumGuardBytes - (64 * KiB);
    for (auto const& row: MemoryLayoutTable)
    {
        CAPTURE(row.name);
        CHECK(isWellFormed(layoutOf(row.row, largestCap, 64 * KiB)));
        CHECK(row.guardBytes <= MaximumGuardBytes);
    }
}

TEST_CASE("MemoryLayout.storeFaultAtomicityPerArchitecture")
{
    // x86-64 faults a straddling store before writing any byte; arm64 does not promise it, and an
    // architecture without a backend is taken as the weaker.
    CHECK(storeFaultAtomicityOf(Architecture::X64) == StoreFaultAtomicity::AllOrNothing);
    CHECK(storeFaultAtomicityOf(Architecture::Arm64) == StoreFaultAtomicity::MayTear);
    CHECK(storeFaultAtomicityOf(Architecture::Other) == StoreFaultAtomicity::MayTear);
    CHECK(StoreFaultAtomicityTable.size() == isoline::platform::ArchitectureCount);
}

TEST_CASE("MemoryLayout.everyRuleReadsOnlyWhatIsThere")
{
    // Each rule is asked only once the rules above it declined, but none relies on that: asked out of
    // order, about explicit mode, a memory64 or custom pages, each still answers from what is there.
    auto const noLayout = std::optional<MemoryLayout> {};
    auto const parity = std::optional<MemoryLayout> { Parity };
    auto const memory = memory32(1, 1);
    auto const wide = memory64(1, 1);
    auto const bytePages = customPages(0, 1, 1);
    auto const explicitQuery = isoline::runtime::StrategyQuery {
        .layout = noLayout, .type = memory, .access = load(0, 1), .atomicity = StoreFaultAtomicity::MayTear
    };
    auto const ruleHolds = [](StrategyRule rule, isoline::runtime::StrategyQuery const& query) {
        return StrategyTable[std::to_underlying(rule)].holds(query);
    };
    // Without a layout there is no guard: every access ends beyond it, and nothing covers an index.
    CHECK(ruleHolds(StrategyRule::BeyondTheGuard, explicitQuery));
    CHECK_FALSE(ruleHolds(StrategyRule::WholeIndexRange, explicitQuery));
    CHECK(ruleHolds(StrategyRule::FoldedOffset, explicitQuery));
    // The whole-index rule checks the address type and the page size itself.
    auto const wideQuery = isoline::runtime::StrategyQuery {
        .layout = parity, .type = wide, .access = load(0, 1), .atomicity = StoreFaultAtomicity::MayTear
    };
    auto const bytePagesQuery = isoline::runtime::StrategyQuery {
        .layout = parity, .type = bytePages, .access = load(0, 1), .atomicity = StoreFaultAtomicity::MayTear
    };
    CHECK_FALSE(ruleHolds(StrategyRule::WholeIndexRange, wideQuery));
    CHECK_FALSE(ruleHolds(StrategyRule::WholeIndexRange, bytePagesQuery));
}
