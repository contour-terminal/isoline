// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/VirtualMemory.hpp>
#include <isoline/platform/testing/RecordingVirtualMemory.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/MemoryLayout.hpp>
#include <isoline/runtime/ReservedMemoryAllocator.hpp>
#include <isoline/runtime/Store.hpp>

using isoline::binary::AddressType;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::platform::Protection;
using isoline::platform::Region;
using isoline::platform::SystemVirtualMemory;
using isoline::platform::VirtualMemoryError;
using isoline::platform::VirtualMemoryFailure;
using isoline::platform::testing::CallOutcome;
using isoline::platform::testing::CallRefusal;
using isoline::platform::testing::RecordingVirtualMemory;
using isoline::platform::testing::VirtualMemoryCall;
using isoline::runtime::ILinearMemory;
using isoline::runtime::layoutOf;
using isoline::runtime::LayoutRow;
using isoline::runtime::MaximumGuardBytes;
using isoline::runtime::MemoryLayout;
using isoline::runtime::MemoryRequest;
using isoline::runtime::memoryRequirementsFor;
using isoline::runtime::Refusal;
using isoline::runtime::ReservedMemoryAllocator;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;

namespace
{

constexpr auto MiB = uint64_t { 1 } << 20;
constexpr auto GiB = uint64_t { 1 } << 30;

/// The default page size, 64 KiB.
constexpr auto PageBytes = isoline::binary::pageSize(MemoryType {});

/// A memory32 of 64 KiB pages, from @p minimumPages to @p maximumPages.
[[nodiscard]] MemoryType memory32(uint64_t minimumPages, uint64_t maximumPages)
{
    return MemoryType { .limits = Limits {
                            .min = minimumPages, .max = maximumPages, .addressType = AddressType::I32 } };
}

/// What the store asks for a memory32 of @p minimumPages to @p maximumPages, guarded as @p layout requires.
[[nodiscard]] MemoryRequest requestOf(MemoryLayout layout, uint64_t minimumPages, uint64_t maximumPages)
{
    auto const type = memory32(minimumPages, maximumPages);
    auto const requirements = memoryRequirementsFor(layout, type);
    return MemoryRequest { .type = type,
                           .initialBytes = minimumPages * PageBytes,
                           .maximumBytes = maximumPages * PageBytes,
                           .requiredGuardBytes = requirements.guardBytes,
                           .requiredReachBytes = requirements.reachBytes };
}

/// The published capped layout for a memory cap of @p capBytes on @p virtualMemory's pages.
[[nodiscard]] MemoryLayout capped(uint64_t capBytes, isoline::platform::IVirtualMemory const& virtualMemory)
{
    return layoutOf(LayoutRow::Capped, capBytes, virtualMemory.pageSize());
}

/// The size of the one reservation @p recording saw made.
[[nodiscard]] uint64_t reservedBytes(RecordingVirtualMemory const& recording)
{
    auto reservations = std::vector<Region> {};
    for (auto const& call: recording.calls())
        if (call.call == VirtualMemoryCall::Reserve && !call.failure.has_value())
            reservations.push_back(call.region);
    REQUIRE(reservations.size() == 1);
    return reservations.front().bytes;
}

/// The bytes @p recording saw committed.
[[nodiscard]] uint64_t committedBytes(RecordingVirtualMemory const& recording)
{
    auto total = uint64_t { 0 };
    for (auto const& call: recording.calls())
        if (call.call == VirtualMemoryCall::Protect && call.protection == Protection::ReadWrite
            && !call.failure.has_value())
            total += call.region.bytes;
    return total;
}

/// The memory @p allocator allocates for @p request, which must succeed.
[[nodiscard]] std::unique_ptr<ILinearMemory> allocated(ReservedMemoryAllocator& allocator,
                                                       MemoryRequest const& request)
{
    auto memory = allocator.allocate(request);
    REQUIRE(memory.has_value());
    REQUIRE(*memory != nullptr);
    return std::move(*memory);
}

/// Whether every byte of @p bytes is zero.
[[nodiscard]] bool allZero(std::span<std::byte const> bytes)
{
    return std::ranges::all_of(bytes, [](std::byte value) { return value == std::byte { 0 }; });
}

/// A refusal of the @p call-th virtual-memory call.
[[nodiscard]] CallRefusal refusalOf(std::size_t call, VirtualMemoryError error)
{
    return CallRefusal { .call = call, .failure = VirtualMemoryFailure { .error = error, .systemCode = 12 } };
}

} // namespace

TEST_CASE("ReservedMemory.neverMovesOnGrow", "[guardpages]")
{
    auto system = SystemVirtualMemory {};
    auto allocator = ReservedMemoryAllocator { system, capped(32 * MiB, system) };
    auto const memory = allocated(allocator, requestOf(capped(32 * MiB, system), 1, 512));
    auto* const base = memory->bytes().data();
    REQUIRE(memory->bytes().size() == PageBytes);
    REQUIRE(allZero(memory->bytes()));
    memory->bytes().front() = std::byte { 0xA5 };
    memory->bytes().back() = std::byte { 0x5A };

    for (auto const pages: std::views::iota(uint64_t { 2 }, uint64_t { 513 }))
    {
        auto const oldBytes = memory->bytes().size();
        REQUIRE(memory->grow(pages * PageBytes).has_value());
        REQUIRE(memory->bytes().data() == base);
        REQUIRE(memory->bytes().size() == pages * PageBytes);
        // The added bytes are zero, and each grown page is written, so the next grow must keep it.
        REQUIRE(allZero(memory->bytes().subspan(oldBytes)));
        memory->bytes().back() = std::byte { 0x5A };
    }
    CHECK(memory->bytes().front() == std::byte { 0xA5 });
    CHECK(memory->bytes()[PageBytes - 1] == std::byte { 0x5A });
}

TEST_CASE("ReservedMemory.guardFollowsEveryGrow", "[guardpages]")
{
    auto system = SystemVirtualMemory {};
    auto recording = RecordingVirtualMemory { system, std::nullopt };
    auto const layout = capped(32 * MiB, recording);
    auto allocator = ReservedMemoryAllocator { recording, layout };
    auto const request = requestOf(layout, 0, 512);
    auto const memory = allocated(allocator, request);
    // 32 MiB of memory and 32 MiB of guard, whole granules on every system.
    auto const reserved = reservedBytes(recording);
    CHECK(reserved == 64 * MiB);

    auto const checkGuard = [&] {
        CHECK(memory->bytes().size() + memory->guardBytes() == reserved);
        CHECK(memory->guardBytes() >= layout.guardBytes);
        CHECK(memory->guardBytes() >= request.requiredGuardBytes);
        CHECK(memory->bytes().size() + memory->guardBytes() >= request.requiredReachBytes);
    };
    checkGuard();
    for (auto const pages: std::views::iota(uint64_t { 1 }, uint64_t { 513 }))
    {
        REQUIRE(memory->grow(pages * PageBytes).has_value());
        checkGuard();
    }
    // At its maximum the memory keeps exactly the layout's guard.
    CHECK(memory->guardBytes() == layout.guardBytes);
    // A grow to the same size commits nothing more.
    auto const commitsBefore = recording.calls().size();
    REQUIRE(memory->grow(512 * PageBytes).has_value());
    CHECK(recording.calls().size() == commitsBefore);
}

TEST_CASE("ReservedMemory.wasmtimeParityReachesFourGiB", "[guardpages]")
{
    auto system = SystemVirtualMemory {};
    auto recording = RecordingVirtualMemory { system, std::nullopt };
    auto const parity = layoutOf(LayoutRow::WasmtimeParity, 4 * GiB, recording.pageSize());
    auto allocator = ReservedMemoryAllocator { recording, parity };
    // A memory32 whose maximum is every page an i32 reaches: its checks are elided, so it relies on the
    // reach.
    auto const request = requestOf(parity, 1, 65'536);
    REQUIRE(request.requiredReachBytes == (4 * GiB) + (32 * MiB));
    auto const memory = allocated(allocator, request);

    CHECK(memory->bytes().size() + memory->guardBytes() >= (4 * GiB) + (32 * MiB));
    CHECK(memory->guardBytes() >= parity.guardBytes);
    CHECK(reservedBytes(recording) == (4 * GiB) + (32 * MiB));
    // Only the first page is committed: the 4 GiB are address space, not memory.
    CHECK(committedBytes(recording) == PageBytes);
    CHECK(committedBytes(recording) < 2 * MiB);
}

TEST_CASE("ReservedMemory.refusalsAreHostMemory", "[guardpages]")
{
    auto system = SystemVirtualMemory {};
    auto const layout = capped(1 * MiB, system);
    auto const request = requestOf(layout, 1, 16);

    SECTION("a refused reservation")
    {
        auto recording = RecordingVirtualMemory { system, refusalOf(1, VirtualMemoryError::ReserveRefused) };
        auto allocator = ReservedMemoryAllocator { recording, layout };
        auto const memory = allocator.allocate(request);
        REQUIRE_FALSE(memory.has_value());
        CHECK(memory.error() == Refusal::HostMemory);
        CHECK(recording.calls().size() == 1);
    }

    SECTION("a refused initial commit releases the reservation")
    {
        auto recording = RecordingVirtualMemory { system, refusalOf(2, VirtualMemoryError::ProtectRefused) };
        auto allocator = ReservedMemoryAllocator { recording, layout };
        auto const memory = allocator.allocate(request);
        REQUIRE_FALSE(memory.has_value());
        CHECK(memory.error() == Refusal::HostMemory);
        auto const calls = recording.calls();
        REQUIRE(calls.size() == 4);
        CHECK(calls[1].outcome == CallOutcome::Refused);
        // The refused commit is undone, then the whole reservation released.
        CHECK(calls[2].call == VirtualMemoryCall::Protect);
        CHECK(calls[2].protection == Protection::None);
        CHECK(calls[2].region == calls[1].region);
        CHECK(calls[3].call == VirtualMemoryCall::Release);
        CHECK(calls[3].region == calls[0].region);
        CHECK_FALSE(calls[3].failure.has_value());
    }

    SECTION("a refused commit on grow leaves the memory unchanged")
    {
        auto recording = RecordingVirtualMemory { system, refusalOf(3, VirtualMemoryError::ProtectRefused) };
        auto allocator = ReservedMemoryAllocator { recording, layout };
        auto const memory = allocated(allocator, request);
        auto* const base = memory->bytes().data();
        auto const guard = memory->guardBytes();
        memory->bytes().back() = std::byte { 7 };

        auto const grown = memory->grow(4 * PageBytes);
        REQUIRE_FALSE(grown.has_value());
        CHECK(grown.error() == Refusal::HostMemory);
        CHECK(memory->bytes().data() == base);
        CHECK(memory->bytes().size() == PageBytes);
        CHECK(memory->guardBytes() == guard);
        CHECK(memory->bytes().back() == std::byte { 7 });
        // The refused range is made inaccessible again: a commit the system began stays no part of it.
        auto const calls = recording.calls();
        REQUIRE(calls.size() == 4);
        CHECK(calls[2].outcome == CallOutcome::Refused);
        CHECK(calls[2].region == Region { .base = base + PageBytes, .bytes = 3 * PageBytes });
        CHECK(calls[3].protection == Protection::None);
        CHECK(calls[3].region == calls[2].region);

        // The next grow commits what the refused one did not.
        REQUIRE(memory->grow(4 * PageBytes).has_value());
        CHECK(memory->bytes().size() == 4 * PageBytes);
        CHECK(allZero(memory->bytes().subspan(PageBytes)));
    }

    SECTION("a grow past the span is refused")
    {
        auto recording = RecordingVirtualMemory { system, std::nullopt };
        auto allocator = ReservedMemoryAllocator { recording, layout };
        auto const memory = allocated(allocator, request);
        // The span is the 1 MiB reservation: one byte past it would eat into the guard.
        auto const callsBefore = recording.calls().size();
        auto const grown = memory->grow((1 * MiB) + 1);
        REQUIRE_FALSE(grown.has_value());
        CHECK(grown.error() == Refusal::HostMemory);
        CHECK(memory->bytes().size() == PageBytes);
        CHECK(recording.calls().size() == callsBefore);
        CHECK(memory->grow(1 * MiB).has_value());
        CHECK(memory->guardBytes() == layout.guardBytes);
    }

    SECTION("a reservation that is no u64 makes no call")
    {
        auto recording = RecordingVirtualMemory { system, std::nullopt };
        auto const maximum = std::numeric_limits<uint64_t>::max();
        auto const page = recording.pageSize();
        // Hostile maxima: one whose pages round past 2^64, and one whose pages leave no room for the guard.
        auto widest = ReservedMemoryAllocator {
            recording, MemoryLayout { .reservationBytes = 0, .guardBytes = MaximumGuardBytes }
        };
        for (auto const maximumBytes: { maximum - 1, maximum - MaximumGuardBytes - page + 2 })
        {
            auto hostile = request;
            hostile.maximumBytes = maximumBytes;
            auto const memory = widest.allocate(hostile);
            REQUIRE_FALSE(memory.has_value());
            CHECK(memory.error() == Refusal::HostMemory);
        }
        CHECK(recording.calls().empty());

        // The last page of the address space, without a guard: no granule past it on a system whose
        // granules are larger than its pages, and a reservation the system refuses on the others.
        auto unguarded = ReservedMemoryAllocator { recording, MemoryLayout {} };
        auto lastPage = request;
        lastPage.maximumBytes = maximum - page + 1;
        auto const memory = unguarded.allocate(lastPage);
        REQUIRE_FALSE(memory.has_value());
        CHECK(memory.error() == Refusal::HostMemory);
        CHECK(std::ranges::none_of(recording.calls(),
                                   [](auto const& call) { return call.call == VirtualMemoryCall::Protect; }));
    }
}

TEST_CASE("ReservedMemory.maximumBeyondReservationReservesTheMaximum", "[guardpages]")
{
    auto system = SystemVirtualMemory {};
    auto recording = RecordingVirtualMemory { system, std::nullopt };
    auto const layout = capped(1 * MiB, recording);
    REQUIRE(layout == MemoryLayout { .reservationBytes = 1 * MiB, .guardBytes = 32 * MiB });
    auto allocator = ReservedMemoryAllocator { recording, layout };
    auto const memory = allocated(allocator, requestOf(layout, 1, 32));
    CHECK(reservedBytes(recording) == (2 * MiB) + (32 * MiB));
    REQUIRE(memory->grow(2 * MiB).has_value());
    CHECK(memory->guardBytes() == 32 * MiB);
}

TEST_CASE("ReservedMemory.reservationRoundsToWholePagesAndGranules", "[guardpages]")
{
    auto system = SystemVirtualMemory {};
    auto recording = RecordingVirtualMemory { system, std::nullopt };
    auto const page = recording.pageSize();
    auto const granule = recording.allocationGranularity();
    // A reservation and a guard of a byte each, a maximum of 0: whole pages, then whole granules.
    auto allocator =
        ReservedMemoryAllocator { recording, MemoryLayout { .reservationBytes = 1, .guardBytes = 1 } };
    auto request = requestOf(MemoryLayout {}, 0, 0);
    auto const memory = allocated(allocator, request);
    auto const expected = ((page + 1 + granule - 1) / granule) * granule;
    CHECK(reservedBytes(recording) == expected);
    CHECK(memory->bytes().empty());
    CHECK(memory->guardBytes() == expected);
    CHECK(committedBytes(recording) == 0);

    // No layout bytes and no maximum still reserve a granule: reserve() takes no empty reservation.
    auto recordingEmpty = RecordingVirtualMemory { system, std::nullopt };
    auto empty = ReservedMemoryAllocator { recordingEmpty, MemoryLayout {} };
    auto const nothing = allocated(empty, request);
    CHECK(reservedBytes(recordingEmpty) == granule);
    CHECK(nothing->guardBytes() == granule);
}

TEST_CASE("ReservedMemory.servesAMixedModule", "[guardpages]")
{
    // One allocator for a memory32, a memory64 and a memory of 1-byte pages, under the guard layout: the
    // first relies on its guard, the others on nothing, and every one is reserved by the same rule.
    auto system = SystemVirtualMemory {};
    auto const layout = capped(1 * MiB, system);
    auto allocator = ReservedMemoryAllocator { system, layout };
    auto store =
        Store { StoreLimits {}, StoreServices { .allocator = allocator, .guardPageLayout = layout } };

    auto const guarded = store.addMemory(memory32(1, 16));
    auto const wide = store.addMemory(
        MemoryType { .limits = Limits { .min = 1, .max = 16, .addressType = AddressType::I64 } });
    auto const bytewise = store.addMemory(MemoryType {
        .limits = Limits { .min = 5, .max = 100, .addressType = AddressType::I32 }, .pageSizeLog2 = 0 });
    REQUIRE(guarded.has_value());
    REQUIRE(wide.has_value());
    REQUIRE(bytewise.has_value());

    CHECK(store.growMemory(*guarded, 15) == 1);
    CHECK(store.memory(*guarded).bytes().size() == 16 * PageBytes);
    CHECK(store.growMemory(*wide, 3) == 1);
    CHECK(store.memory(*wide).bytes().size() == 4 * PageBytes);

    // Each grow succeeded through the store's own check that the memory still provides what its code
    // relies on (provides()).
    auto& bytes = store.memory(*bytewise);
    REQUIRE(bytes.bytes().size() == 5);
    CHECK(store.growMemory(*bytewise, 2) == 5);
    REQUIRE(bytes.bytes().size() == 7);
    CHECK(allZero(bytes.bytes()));
}

TEST_CASE("ReservedMemory.customPagesDeclareNoGuardMidPage", "[guardpages]")
{
    // A memory of 1-byte pages: while its size ends mid-page, the committed rest of that page follows its
    // bytes and is accessible, so it declares no guard (its requirements are {0, 0}). At a whole system
    // page its guard is the reservation past it.
    auto system = SystemVirtualMemory {};
    auto recording = RecordingVirtualMemory { system, std::nullopt };
    auto const layout = capped(1 * MiB, recording);
    auto allocator = ReservedMemoryAllocator { recording, layout };
    auto const page = recording.pageSize();
    auto const type =
        MemoryType { .limits = Limits { .min = 5, .max = page, .addressType = AddressType::I32 },
                     .pageSizeLog2 = 0 };
    auto const memory =
        allocated(allocator, MemoryRequest { .type = type, .initialBytes = 5, .maximumBytes = page });
    auto const reserved = reservedBytes(recording);
    CHECK(memory->bytes().size() == 5);
    CHECK(memory->guardBytes() == 0);
    CHECK(committedBytes(recording) == page);
    REQUIRE(memory->grow(100).has_value());
    CHECK(memory->guardBytes() == 0);
    REQUIRE(memory->grow(page).has_value());
    CHECK(memory->guardBytes() == reserved - page);
    CHECK(memory->guardBytes() >= layout.guardBytes);
    CHECK(committedBytes(recording) == page);
}
