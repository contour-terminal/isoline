// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/platform/HardwareCapabilities.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/PlatformTable.hpp>
#include <isoline/platform/ThisPlatform.hpp>
#include <isoline/platform/VirtualMemory.hpp>
#include <isoline/platform/testing/BackedMemory.hpp>
#include <isoline/platform/testing/RecordingVirtualMemory.hpp>

using namespace isoline::platform;
using isoline::platform::testing::backedBytes;
using isoline::platform::testing::CallOutcome;
using isoline::platform::testing::CallRefusal;
using isoline::platform::testing::RecordingVirtualMemory;
using isoline::platform::testing::VirtualMemoryCall;

namespace
{

/// A page-aligned address no process can map: past the end of every user address space (47, 48 or
/// 57 bits on x86-64, 48 or 52 on arm64), under emulators too.
[[nodiscard]] Region unmappableRegion(uint64_t pageSize) noexcept
{
    return Region { .base = std::bit_cast<std::byte*>(uintptr_t { 1 } << 63U), .bytes = pageSize };
}

/// Two pages from the last page of the address space: a range that wraps around its end, which
/// every system refuses to unmap (macOS's munmap accepts a range merely beyond the user address space).
[[nodiscard]] Region wrappingRegion(uint64_t pageSize) noexcept
{
    return Region { .base =
                        std::bit_cast<std::byte*>(~uintptr_t { 0 } - static_cast<uintptr_t>(pageSize) + 1),
                    .bytes = 2 * pageSize };
}

/// The bytes of @p region.
[[nodiscard]] std::span<std::byte> bytesOf(Region region) noexcept
{
    return { region.base, static_cast<std::size_t>(region.bytes) };
}

/// The decommit mechanisms that work on this system: on Linux both POSIX ones, on macOS and FreeBSD only
/// the fresh mapping, on Windows its own.
[[nodiscard]] std::vector<DecommitKind> decommitKindsOfThisSystem()
{
    auto kinds = std::vector<DecommitKind> {};
    for (auto const kind: { DecommitKind::MadviseDontNeed,
                            DecommitKind::MapFixedAnonymous,
                            DecommitKind::VirtualFreeDecommit })
    {
        if (decommitsOnThisSystem(kind))
            kinds.push_back(kind);
    }
    return kinds;
}

/// Writes a non-zero byte into every page of @p region, so that each is backed.
void touchEveryPage(Region region, uint64_t pageSize) noexcept
{
    auto const bytes = bytesOf(region);
    for (auto const offset: std::views::iota(uint64_t { 0 }, region.bytes / pageSize))
        bytes[static_cast<std::size_t>(offset * pageSize)] = std::byte { 0xA5 };
}

} // namespace

TEST_CASE("VirtualMemory.protectionHasNoWritableExecutable")
{
    // W^X is a property of the type: no enumerator, and so no row, is both. ProtectionTable's
    // static_assert enforces it; the test documents it.
    STATIC_CHECK(std::ranges::none_of(allProtections(), isWritableAndExecutable));
    for (auto const protection: allProtections())
    {
        auto const& row = protectionRow(protection);
        INFO("protection " << std::to_underlying(protection));
        CHECK(row.protection == protection);
        CHECK_FALSE(isWritableAndExecutable(protection));
        // Executable pages are always readable, writable ones always committed by the protection.
        if (row.execute == PageAccess::Allowed)
            CHECK(row.read == PageAccess::Allowed);
        if (row.write == PageAccess::Allowed)
            CHECK(row.commitment == Commitment::Commit);
        // Only Protection::None discards contents.
        CHECK((row.commitment == Commitment::Decommit) == (protection == Protection::None));
    }
}

TEST_CASE("VirtualMemory.reservedIsInaccessibleCommittedIsZeroed")
{
    auto memory = SystemVirtualMemory {};
    auto const pageSize = memory.pageSize();
    REQUIRE(std::has_single_bit(pageSize));
    REQUIRE(memory.allocationGranularity() % pageSize == 0);
    auto const reservation = memory.reserve(1U << 20U);
    REQUIRE(reservation.has_value());
    CHECK(reservation->bytes == 1U << 20U);
    CHECK(std::bit_cast<uintptr_t>(reservation->base) % memory.allocationGranularity() == 0);
    // Reserved, it is backed by nothing.
    CHECK(backedBytes(*reservation, pageSize) == 0);

    auto const firstPage = Region { .base = reservation->base, .bytes = pageSize };
    REQUIRE(memory.protect(firstPage, Protection::ReadWrite).has_value());
    CHECK(std::ranges::all_of(bytesOf(firstPage), [](std::byte value) { return value == std::byte { 0 }; }));
    touchEveryPage(firstPage, pageSize);
    CHECK(bytesOf(firstPage)[0] == std::byte { 0xA5 });
    CHECK(backedBytes(*reservation, pageSize) == pageSize);

    // Read-only and back keep the contents; None discards them, and a new commit reads zeroes again.
    REQUIRE(memory.protect(firstPage, Protection::Read).has_value());
    CHECK(bytesOf(firstPage)[0] == std::byte { 0xA5 });
    REQUIRE(memory.protect(firstPage, Protection::None).has_value());
    CHECK(backedBytes(*reservation, pageSize) == 0);
    REQUIRE(memory.protect(firstPage, Protection::ReadWrite).has_value());
    CHECK(bytesOf(firstPage)[0] == std::byte { 0 });

    REQUIRE(memory.release(*reservation).has_value());
}

TEST_CASE("VirtualMemory.reserveRefusalIsReported")
{
    auto system = SystemVirtualMemory {};
    auto const refusal =
        VirtualMemoryFailure { .error = VirtualMemoryError::ReserveRefused, .systemCode = 12 };
    auto memory = RecordingVirtualMemory { system, CallRefusal { .call = 1, .failure = refusal } };
    auto const granule = memory.allocationGranularity();

    // The first call is refused and never reaches the system; the second passes.
    auto const refused = memory.reserve(granule);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == refusal);
    auto const reservation = memory.reserve(granule);
    REQUIRE(reservation.has_value());
    REQUIRE(memory.protect(*reservation, Protection::ReadWrite).has_value());
    REQUIRE(memory.release(*reservation).has_value());

    auto const calls = memory.calls();
    REQUIRE(calls.size() == 4);
    CHECK(calls[0].call == VirtualMemoryCall::Reserve);
    CHECK(calls[0].outcome == CallOutcome::Refused);
    CHECK(calls[0].region == Region { .base = nullptr, .bytes = granule });
    CHECK(calls[0].failure == refusal);
    CHECK(calls[1].call == VirtualMemoryCall::Reserve);
    CHECK(calls[1].outcome == CallOutcome::Forwarded);
    CHECK(calls[1].region == *reservation);
    CHECK_FALSE(calls[1].failure.has_value());
    CHECK(calls[2].call == VirtualMemoryCall::Protect);
    CHECK(calls[2].protection == Protection::ReadWrite);
    CHECK_FALSE(calls[2].decommit.has_value());
    CHECK(calls[3].call == VirtualMemoryCall::Release);
    CHECK(calls[3].region == *reservation);

    // The system's own refusal carries its code: no address space holds 2^62 bytes.
    auto const tooLarge = system.reserve(uint64_t { 1 } << 62U);
    REQUIRE_FALSE(tooLarge.has_value());
    CHECK(tooLarge.error().error == VirtualMemoryError::ReserveRefused);
    CHECK(tooLarge.error().systemCode != 0);
}

TEST_CASE("VirtualMemory.refusalsOfEveryCallCarryTheSystemCode")
{
    // Pages nobody can map: every commitment path (commit, retain, decommit) and the release fail with
    // the system's code, never as a success.
    auto memory = SystemVirtualMemory {};
    auto const nowhere = unmappableRegion(memory.pageSize());
    for (auto const protection:
         { Protection::ReadWrite, Protection::Read, Protection::ReadExecute, Protection::None })
    {
        INFO("protection " << std::to_underlying(protection));
        auto const result = memory.protect(nowhere, protection);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == VirtualMemoryError::ProtectRefused);
        CHECK(result.error().systemCode != 0);
    }
    auto const released = memory.release(wrappingRegion(memory.pageSize()));
    REQUIRE_FALSE(released.has_value());
    CHECK(released.error().error == VirtualMemoryError::ReleaseRefused);
    CHECK(released.error().systemCode != 0);
}

TEST_CASE("VirtualMemory.branchTargetsFollowThePlatformRow")
{
    // ReadExecuteBranchTargets is PROT_BTI where the row has it and the processor supports it, and a
    // refusal without a system call (code 0) on every other platform.
    auto memory = SystemVirtualMemory {};
    auto const reservation = memory.reserve(memory.allocationGranularity());
    REQUIRE(reservation.has_value());
    auto const page = Region { .base = reservation->base, .bytes = memory.pageSize() };
    REQUIRE(memory.protect(page, Protection::ReadWrite).has_value());
    auto const sealed = memory.protect(page, Protection::ReadExecuteBranchTargets);
    auto const capabilities = makeSystemHardwareCapabilities();
    if (ThisPlatform.branchTargets == BranchTargetProtectionKind::None)
    {
        REQUIRE_FALSE(sealed.has_value());
        CHECK(sealed.error()
              == VirtualMemoryFailure { .error = VirtualMemoryError::BranchTargetsUnsupported,
                                        .systemCode = 0 });
    }
    else if (capabilities->branchTargetIdentification() == CapabilityState::Supported)
    {
        CHECK(sealed.has_value());
    }
    else
    {
        REQUIRE_FALSE(sealed.has_value());
        CHECK(sealed.error().error == VirtualMemoryError::BranchTargetsUnsupported);
    }
    REQUIRE(memory.release(*reservation).has_value());
}

TEST_CASE("VirtualMemory.largeReservationCommitsNothing", "[guardpages][memory]")
{
    // A Wasmtime-parity reservation (4 GiB + a 32 MiB guard) is address space only; committing part of it
    // backs that part, and decommitting it gives every page back.
    auto memory = SystemVirtualMemory {};
    auto const pageSize = memory.pageSize();
    constexpr auto ReservationBytes = (uint64_t { 4 } << 30U) + (uint64_t { 32 } << 20U);
    constexpr auto CommittedBytes = uint64_t { 16 } << 20U;
    auto const reservation = memory.reserve(ReservationBytes);
    REQUIRE(reservation.has_value());
    CHECK(backedBytes(*reservation, pageSize) == 0);

    auto const committed = Region { .base = reservation->base, .bytes = CommittedBytes };
    REQUIRE(memory.protect(committed, Protection::ReadWrite).has_value());
    touchEveryPage(committed, pageSize);
    CHECK(backedBytes(*reservation, pageSize) == CommittedBytes);
    REQUIRE(memory.protect(committed, Protection::None).has_value());
    CHECK(backedBytes(*reservation, pageSize) == 0);

    REQUIRE(memory.release(*reservation).has_value());
}

TEST_CASE("VirtualMemory.decommitReadsZeroesOnEveryRow")
{
    // Whatever the row's mechanism, a decommitted page holds nothing of what was written to it: the
    // next commit reads zeroes, and until then it is backed by nothing.
    for (auto const decommit: decommitKindsOfThisSystem())
    {
        INFO("decommit " << std::to_underlying(decommit));
        auto system = SystemVirtualMemory { decommit };
        auto memory = RecordingVirtualMemory { system, std::nullopt };
        CHECK(memory.decommitKind() == decommit);
        auto const pageSize = memory.pageSize();
        auto const reservation = memory.reserve(memory.allocationGranularity());
        REQUIRE(reservation.has_value());
        auto const pages = Region { .base = reservation->base, .bytes = reservation->bytes };
        REQUIRE(memory.protect(pages, Protection::ReadWrite).has_value());
        std::ranges::fill(bytesOf(pages), std::byte { 0x5A });
        REQUIRE(memory.protect(pages, Protection::None).has_value());
        CHECK(backedBytes(pages, pageSize) == 0);
        REQUIRE(memory.protect(pages, Protection::ReadWrite).has_value());
        CHECK(std::ranges::all_of(bytesOf(pages), [](std::byte value) { return value == std::byte { 0 }; }));
        REQUIRE(memory.release(*reservation).has_value());

        auto const calls = memory.calls();
        REQUIRE(calls.size() == 5);
        CHECK(calls[2].protection == Protection::None);
        CHECK(calls[2].decommit == decommit);

        // A refused decommit is reported, by every mechanism.
        auto const refused = memory.protect(unmappableRegion(pageSize), Protection::None);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().error == VirtualMemoryError::ProtectRefused);
        CHECK(refused.error().systemCode != 0);
        // The record holds the system's own failure of a forwarded call.
        REQUIRE(memory.calls().size() == 6);
        CHECK(memory.calls()[5].outcome == CallOutcome::Forwarded);
        CHECK(memory.calls()[5].failure == refused.error());
    }
}

TEST_CASE("VirtualMemory.decommitMechanismsPerSystem")
{
    // MADV_DONTNEED keeps a private mapping's contents on macOS and FreeBSD, so only their own mechanism,
    // the fresh mapping, decommits there; Linux has both, Windows its own alone.
    auto const decommitsOnSystem = [](OperatingSystem system, DecommitKind kind) {
        return decommitsOn(platformRow(system, Architecture::Arm64), kind);
    };
    CHECK(decommitsOnSystem(OperatingSystem::Linux, DecommitKind::MadviseDontNeed));
    CHECK(decommitsOnSystem(OperatingSystem::Linux, DecommitKind::MapFixedAnonymous));
    CHECK_FALSE(decommitsOnSystem(OperatingSystem::Linux, DecommitKind::VirtualFreeDecommit));
    for (auto const system: { OperatingSystem::MacOS, OperatingSystem::FreeBSD })
    {
        INFO("system " << std::to_underlying(system));
        CHECK_FALSE(decommitsOnSystem(system, DecommitKind::MadviseDontNeed));
        CHECK(decommitsOnSystem(system, DecommitKind::MapFixedAnonymous));
        CHECK_FALSE(decommitsOnSystem(system, DecommitKind::VirtualFreeDecommit));
    }
    CHECK_FALSE(decommitsOnSystem(OperatingSystem::Windows, DecommitKind::MadviseDontNeed));
    CHECK_FALSE(decommitsOnSystem(OperatingSystem::Windows, DecommitKind::MapFixedAnonymous));
    CHECK(decommitsOnSystem(OperatingSystem::Windows, DecommitKind::VirtualFreeDecommit));
    CHECK(decommitsOnThisSystem(ThisPlatform.decommit));
}

TEST_CASE("VirtualMemory.sizesBeyondTheSizeTypeAreRefused")
{
    // A 32-bit system's calls take a 32-bit size: what does not fit is refused with the call's error and
    // no system code, never truncated.
    using isoline::platform::detail::systemSize;
    auto const fits = systemSize<uint32_t>(0xFFFF'FFFFU, VirtualMemoryError::ReserveRefused);
    REQUIRE(fits.has_value());
    CHECK(*fits == 0xFFFF'FFFFU);
    for (auto const error: { VirtualMemoryError::ReserveRefused,
                             VirtualMemoryError::ProtectRefused,
                             VirtualMemoryError::ReleaseRefused })
    {
        for (auto const bytes: { uint64_t { 1 } << 32U, ~uint64_t { 0 } })
        {
            auto const refused = systemSize<uint32_t>(bytes, error);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error() == VirtualMemoryFailure { .error = error, .systemCode = 0 });
        }
    }
    auto const wide = systemSize<uint64_t>(~uint64_t { 0 }, VirtualMemoryError::ReserveRefused);
    REQUIRE(wide.has_value());
    CHECK(*wide == ~uint64_t { 0 });
}
