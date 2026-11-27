// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <utility>

#include <isoline/meta/EnumRows.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/PlatformTable.hpp>
#include <isoline/platform/ThisPlatform.hpp>

namespace isoline::platform
{

/// What a range of pages may be accessed for. There is deliberately no enumerator that is both
/// writable and executable: W^X is a property of this type (ProtectionTable checks it).
enum class Protection : uint8_t
{
    None,                     ///< Inaccessible, and decommitted: its contents are gone.
    Read,                     ///< Readable.
    ReadWrite,                ///< Readable and writable (committed, zeroed when first committed).
    ReadExecute,              ///< Readable and executable.
    ReadExecuteBranchTargets, ///< Readable and executable, with branch-target landing pads enforced.
};

/// The last Protection enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastProtection = Protection::ReadExecuteBranchTargets;

/// Whether a protection allows one kind of access.
enum class PageAccess : uint8_t
{
    Denied,  ///< The access faults.
    Allowed, ///< The access succeeds.
};

/// The last PageAccess enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastPageAccess = PageAccess::Allowed;

/// Whether a protection enforces branch-target landing pads.
enum class BranchTargetEnforcement : uint8_t
{
    Off, ///< Any instruction may be a branch target.
    On,  ///< Indirect branches must land on a landing pad (arm64 BTI).
};

/// The last BranchTargetEnforcement enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastBranchTargetEnforcement = BranchTargetEnforcement::On;

/// What making pages of a reservation take a protection does to their backing memory.
enum class Commitment : uint8_t
{
    Decommit, ///< Their memory is returned to the system and their contents discarded.
    Commit,   ///< They are backed by memory, zeroed when first committed.
    Retain,   ///< Only their protection changes: they must be committed already.
};

/// The last Commitment enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastCommitment = Commitment::Retain;

/// What one Protection allows: one row of ProtectionTable.
struct ProtectionRow
{
    Protection protection = Protection::None; ///< The protection.
    PageAccess read = PageAccess::Denied;     ///< Whether loads succeed.
    PageAccess write = PageAccess::Denied;    ///< Whether stores succeed.
    PageAccess execute = PageAccess::Denied;  ///< Whether instruction fetch succeeds.
    /// Whether landing pads are enforced.
    BranchTargetEnforcement branchTargets = BranchTargetEnforcement::Off;
    Commitment commitment = Commitment::Retain; ///< What it does to the pages' backing memory.
};

/// Every Protection, at its enumerator's index.
inline constexpr auto ProtectionTable = std::to_array<ProtectionRow>({
    { .protection = Protection::None, .commitment = Commitment::Decommit },
    { .protection = Protection::Read, .read = PageAccess::Allowed },
    { .protection = Protection::ReadWrite,
      .read = PageAccess::Allowed,
      .write = PageAccess::Allowed,
      .commitment = Commitment::Commit },
    { .protection = Protection::ReadExecute, .read = PageAccess::Allowed, .execute = PageAccess::Allowed },
    { .protection = Protection::ReadExecuteBranchTargets,
      .read = PageAccess::Allowed,
      .execute = PageAccess::Allowed,
      .branchTargets = BranchTargetEnforcement::On },
});

static_assert(meta::firstMisplacedRow<LastProtection>(ProtectionTable, &ProtectionRow::protection)
                  == meta::NoMisplacedRow,
              "ProtectionTable has one row per enumerator, at its index");

/// The row of @p protection.
/// @param protection The protection.
/// @return Its row of ProtectionTable; @p protection must be an enumerator (checked).
[[nodiscard]] constexpr ProtectionRow const& protectionRow(Protection protection) noexcept
{
    Require(std::to_underlying(protection) < ProtectionTable.size());
    return ProtectionTable[std::to_underlying(protection)];
}

/// Whether @p protection allows both stores and instruction fetch.
/// @param protection The protection.
/// @return Whether it is writable and executable at once.
[[nodiscard]] constexpr bool isWritableAndExecutable(Protection protection) noexcept
{
    auto const& row = protectionRow(protection);
    return row.write == PageAccess::Allowed && row.execute == PageAccess::Allowed;
}

/// Every Protection enumerator, in order.
/// @return The protections.
[[nodiscard]] constexpr std::array<Protection, ProtectionTable.size()> allProtections() noexcept
{
    auto protections = std::array<Protection, ProtectionTable.size()> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, ProtectionTable.size()))
        protections[index] = ProtectionTable[index].protection;
    return protections;
}

static_assert(std::ranges::none_of(allProtections(), isWritableAndExecutable),
              "no Protection is writable and executable at once");

/// The protection flag that makes executable pages enforce branch-target landing pads, for one
/// BranchTargetProtectionKind: one row of BranchTargetProtectionFlags.
struct BranchTargetProtectionFlagRow
{
    BranchTargetProtectionKind kind = BranchTargetProtectionKind::None; ///< The platform's kind.
    /// Its mmap/mprotect flag; 0 where there is none, so ReadExecuteBranchTargets is refused without a
    /// system call.
    int flag = 0;
};

/// Every BranchTargetProtectionKind, at its enumerator's index. PROT_BTI is Linux's arm64 ABI
/// (arch/arm64/include/uapi/asm/mman.h), spelled here because only arm64 headers define it; Linux
/// arm64's row (HardwareCapabilitiesLinuxArm64.cpp) checks it against theirs.
inline constexpr auto BranchTargetProtectionFlags = std::to_array<BranchTargetProtectionFlagRow>({
    { .kind = BranchTargetProtectionKind::None, .flag = 0 },
    { .kind = BranchTargetProtectionKind::LinuxProtBti, .flag = 0x10 },
});

static_assert(meta::firstMisplacedRow<LastBranchTargetProtectionKind>(BranchTargetProtectionFlags,
                                                                      &BranchTargetProtectionFlagRow::kind)
                  == meta::NoMisplacedRow,
              "BranchTargetProtectionFlags has one row per enumerator, at its index");

/// Whether @p kind decommits on @p row's system: its own row's mechanism, and on every POSIX system a
/// fresh MAP_FIXED mapping, which works wherever mmap does. MADV_DONTNEED discards pages on Linux only.
/// @param row The platform.
/// @param kind The mechanism.
/// @return Whether it makes a recommitted page read zeroes there.
[[nodiscard]] constexpr bool decommitsOn(PlatformRow const& row, DecommitKind kind) noexcept
{
    return kind == row.decommit
           || (kind == DecommitKind::MapFixedAnonymous && row.system != OperatingSystem::Windows);
}

/// Whether @p kind decommits on the system this build runs on (decommitsOn() of ThisPlatform).
/// @param kind The mechanism.
/// @return Whether it does.
[[nodiscard]] constexpr bool decommitsOnThisSystem(DecommitKind kind) noexcept
{
    return decommitsOn(ThisPlatform, kind);
}

/// A range of address space: what IVirtualMemory::reserve() returns, or a page-aligned part of it.
struct Region
{
    std::byte* base = nullptr; ///< Its first byte.
    uint64_t bytes = 0;        ///< Its size.

    /// Regions are equal when they span the same bytes.
    [[nodiscard]] friend constexpr bool operator==(Region const&, Region const&) noexcept = default;
};

/// Why a virtual-memory call failed.
enum class VirtualMemoryError : uint8_t
{
    ReserveRefused,           ///< The system refused to reserve the address space.
    ProtectRefused,           ///< The system refused to change the protection (or to commit).
    ReleaseRefused,           ///< The system refused to release the reservation.
    BranchTargetsUnsupported, ///< ReadExecuteBranchTargets on a platform or kernel without it.
};

/// The last VirtualMemoryError enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastVirtualMemoryError = VirtualMemoryError::BranchTargetsUnsupported;

/// A failed virtual-memory call: why, and the system's own code for it.
struct VirtualMemoryFailure
{
    VirtualMemoryError error = VirtualMemoryError::ReserveRefused; ///< Why.
    /// The system's error code: errno on POSIX, GetLastError() on Windows; 0 when no system call was made.
    int systemCode = 0;

    /// Failures are equal when both their error and their system code are.
    [[nodiscard]] friend constexpr bool operator==(VirtualMemoryFailure const&,
                                                   VirtualMemoryFailure const&) noexcept = default;
};

/// The process's address space: reserving, protecting (committing and decommitting) and releasing
/// pages. Every mapping Isoline makes goes through it, so a test can inject a refusal or record the
/// transitions.
class IVirtualMemory
{
  public:
    virtual ~IVirtualMemory() = default;

    /// The size of a page, which protect() works in.
    /// @return The page size in bytes, a power of two.
    [[nodiscard]] virtual uint64_t pageSize() const noexcept = 0;

    /// The granularity reservations are made in: 64 KiB on Windows, the page size elsewhere.
    /// @return The allocation granularity in bytes, a multiple of pageSize().
    [[nodiscard]] virtual uint64_t allocationGranularity() const noexcept = 0;

    /// How protect(…, Protection::None) decommits pages: the platform row's mechanism.
    /// @return It.
    [[nodiscard]] virtual DecommitKind decommitKind() const noexcept = 0;

    /// Reserves @p bytes of address space, inaccessible (Protection::None), committing nothing.
    /// @param bytes The size, a non-zero multiple of allocationGranularity() (checked).
    /// @return The reserved region; or ReserveRefused with the system's code, or with code 0 when
    ///         @p bytes does not fit the system's size type.
    [[nodiscard]] virtual std::expected<Region, VirtualMemoryFailure> reserve(uint64_t bytes) noexcept = 0;

    /// Gives the pages of @p region @p protection, with the protection's Commitment: ReadWrite commits
    /// them (zeroed when first committed), None decommits them, discarding their contents, and every
    /// other protection applies to pages already committed.
    /// @param region Pages of a reservation: its base and size multiples of pageSize(), not empty (checked).
    /// @param protection The protection.
    /// @return Nothing; or ProtectRefused with the system's code (0 when the size does not fit the
    ///         system's size type), or BranchTargetsUnsupported when @p protection is
    ///         ReadExecuteBranchTargets and the platform or its kernel lacks it.
    [[nodiscard]] virtual std::expected<void, VirtualMemoryFailure> protect(
        Region region, Protection protection) noexcept = 0;

    /// Releases a reservation: the address space is the system's again.
    /// @param region A region reserve() returned, whole: page-aligned and not empty (checked), and the
    ///        entire reservation (checked on Windows, where the system can tell, and by
    ///        testing::RecordingVirtualMemory on every platform).
    /// @return Nothing; or ReleaseRefused with the system's code (0 when the size does not fit the
    ///         system's size type).
    [[nodiscard]] virtual std::expected<void, VirtualMemoryFailure> release(Region region) noexcept = 0;
};

namespace detail
{

    /// Checks reserve()'s precondition: @p bytes is a non-zero multiple of @p granularity.
    /// @param bytes The size to reserve.
    /// @param granularity The allocation granularity.
    void requireReservationSize(uint64_t bytes, uint64_t granularity) noexcept;

    /// Checks protect()'s and release()'s precondition: @p region is whole pages of @p pageSize, not empty.
    /// @param region The region.
    /// @param pageSize The page size.
    void requirePageRange(Region const& region, uint64_t pageSize) noexcept;

    /// Checks release()'s precondition: @p region is the whole of @p reservation, the reservation its
    /// base lies in.
    /// @param region The region to release.
    /// @param reservation The reservation, as reserve() returned it; empty when there is none.
    void requireWholeReservation(Region const& region, Region const& reservation) noexcept;

    /// @p bytes as a @p SizeType, the size type of the system's calls: a refusal with @p error (and
    /// system code 0, no call made) when it does not fit, as on a 32-bit system for 4 GiB or more.
    /// @param bytes The size.
    /// @param error What the call refuses with.
    /// @return It, narrowed; or the refusal.
    template <typename SizeType>
    [[nodiscard]] constexpr std::expected<SizeType, VirtualMemoryFailure> systemSize(
        uint64_t bytes, VirtualMemoryError error) noexcept
    {
        if (!std::in_range<SizeType>(bytes))
            return std::unexpected { VirtualMemoryFailure { .error = error, .systemCode = 0 } };
        return static_cast<SizeType>(bytes);
    }

} // namespace detail

/// The operating system's virtual memory: mmap/mprotect/madvise/munmap on POSIX, VirtualAlloc/
/// VirtualProtect/VirtualFree on Windows (the CMake-selected VirtualMemory{Posix,Windows}.cpp).
/// ReadExecuteBranchTargets is PROT_BTI where this build's platform row has it (Linux arm64) and
/// BranchTargetsUnsupported, without a system call, everywhere else. Protection::None decommits by
/// the row's DecommitKind.
class SystemVirtualMemory final: public IVirtualMemory
{
  public:
    /// Reads the page size and the allocation granularity from the system; decommits as this build's
    /// platform row does (ThisPlatform.decommit).
    SystemVirtualMemory() noexcept;

    /// As the default constructor, decommitting with @p decommit instead: a test runs another POSIX
    /// row's mechanism on this system.
    /// @param decommit The mechanism; one that decommits on this system (checked: decommitsOnThisSystem()).
    explicit SystemVirtualMemory(DecommitKind decommit) noexcept;

    /// @copydoc IVirtualMemory::decommitKind
    [[nodiscard]] DecommitKind decommitKind() const noexcept override { return _decommit; }

    /// @copydoc IVirtualMemory::pageSize
    [[nodiscard]] uint64_t pageSize() const noexcept override { return _pageSize; }

    /// @copydoc IVirtualMemory::allocationGranularity
    [[nodiscard]] uint64_t allocationGranularity() const noexcept override { return _allocationGranularity; }

    /// @copydoc IVirtualMemory::reserve
    [[nodiscard]] std::expected<Region, VirtualMemoryFailure> reserve(uint64_t bytes) noexcept override;

    /// @copydoc IVirtualMemory::protect
    [[nodiscard]] std::expected<void, VirtualMemoryFailure> protect(Region region,
                                                                    Protection protection) noexcept override;

    /// @copydoc IVirtualMemory::release
    [[nodiscard]] std::expected<void, VirtualMemoryFailure> release(Region region) noexcept override;

  private:
    uint64_t _pageSize;              ///< The system's page size.
    uint64_t _allocationGranularity; ///< The system's reservation granularity.
    DecommitKind _decommit;          ///< How Protection::None decommits.
};

} // namespace isoline::platform
