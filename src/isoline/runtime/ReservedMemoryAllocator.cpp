// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/ReservedMemoryAllocator.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>

#include <isoline/meta/Check.hpp>
#include <isoline/platform/VirtualMemory.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/MemoryLayout.hpp>

namespace isoline::runtime
{

namespace
{
    /// @p bytes rounded up to a multiple of @p alignment.
    /// @param bytes The size.
    /// @param alignment The multiple, not 0.
    /// @return It; or std::nullopt when it is no u64.
    [[nodiscard]] constexpr std::optional<uint64_t> roundedUp(uint64_t bytes, uint64_t alignment) noexcept
    {
        auto const shortfall = (alignment - (bytes % alignment)) % alignment;
        if (bytes > std::numeric_limits<uint64_t>::max() - shortfall)
            return std::nullopt;
        return bytes + shortfall;
    }

    /// The bytes a memory may span, and the reservation that holds them and the guard after them.
    struct ReservationExtent
    {
        uint64_t spanBytes = 0;     ///< What the memory may commit: whole pages from its base.
        uint64_t reservedBytes = 0; ///< The whole reservation: the span, the guard, whole granules.
    };

    /// The reservation of a memory that may grow to @p maximumBytes under @p layout: the larger of the
    /// layout's reservation and @p maximumBytes as whole pages, and the guard after it, as whole granules.
    /// Never empty, since reserve() takes no empty reservation.
    /// @param layout The layout.
    /// @param maximumBytes The request's maximum.
    /// @param virtualMemory The address space, for its page size and allocation granularity.
    /// @return The sizes; or std::nullopt when the reservation is no u64.
    [[nodiscard]] std::optional<ReservationExtent> reservationExtentFor(
        MemoryLayout layout, uint64_t maximumBytes, platform::IVirtualMemory const& virtualMemory) noexcept
    {
        auto const span =
            roundedUp(std::max(layout.reservationBytes, maximumBytes), virtualMemory.pageSize());
        if (!span || *span > std::numeric_limits<uint64_t>::max() - layout.guardBytes)
            return std::nullopt;
        return roundedUp(std::max(*span + layout.guardBytes, uint64_t { 1 }),
                         virtualMemory.allocationGranularity())
            .transform([&](uint64_t reserved) {
                return ReservationExtent { .spanBytes = *span, .reservedBytes = reserved };
            });
    }

    /// Commits @p region. A refused commit is undone whole, because the system may have committed part of
    /// the range before it refused (mprotect changes pages one mapping at a time): every page past the
    /// memory's committed ones stays inaccessible, as its guard promises.
    ///
    /// Invariant (checked): the system decommits a range it just refused to commit.
    /// @param virtualMemory The address space.
    /// @param region Uncommitted pages of a reservation.
    /// @return Nothing; or HostMemory, the pages inaccessible again.
    [[nodiscard]] std::expected<void, Refusal> commit(platform::IVirtualMemory& virtualMemory,
                                                      platform::Region region) noexcept
    {
        if (virtualMemory.protect(region, platform::Protection::ReadWrite))
            return {};
        auto const restored = virtualMemory.protect(region, platform::Protection::None);
        ISOLINE_INVARIANT(restored.has_value());
        return std::unexpected { Refusal::HostMemory };
    }

    /// A linear memory in a reservation of its own: committed from its base in whole pages, grown in place,
    /// released when destroyed.
    class ReservedLinearMemory final: public ILinearMemory
    {
      public:
        /// @param virtualMemory The address space the reservation was made in.
        /// @param reservation The reservation, which the memory owns from now on, nothing of it committed.
        /// @param spanBytes The most the memory may commit, whole pages: the reservation past it is the
        /// guard.
        ReservedLinearMemory(platform::IVirtualMemory& virtualMemory,
                             platform::Region reservation,
                             uint64_t spanBytes) noexcept:
            _virtualMemory { &virtualMemory }, _reservation { reservation }, _spanBytes { spanBytes }
        {
        }

        ReservedLinearMemory(ReservedLinearMemory const&) = delete;
        ReservedLinearMemory(ReservedLinearMemory&&) = delete;
        ReservedLinearMemory& operator=(ReservedLinearMemory const&) = delete;
        ReservedLinearMemory& operator=(ReservedLinearMemory&&) = delete;

        /// Releases the reservation. A refused release only leaks address space, which nobody is left to be
        /// told about.
        ~ReservedLinearMemory() override { std::ignore = _virtualMemory->release(_reservation); }

        [[nodiscard]] std::span<std::byte> bytes() noexcept override
        {
            return { _reservation.base, static_cast<std::size_t>(_size) };
        }

        [[nodiscard]] std::span<std::byte const> bytes() const noexcept override
        {
            return { _reservation.base, static_cast<std::size_t>(_size) };
        }

        /// The reservation past the committed pages, when bytes() ends where they do, as a memory of 64 KiB
        /// pages always does. Otherwise 0: the committed tail of the last page follows bytes() and is
        /// accessible, and only a memory whose requirements are {0, 0} (custom pages) ends mid-page.
        [[nodiscard]] uint64_t guardBytes() const noexcept override
        {
            return _size == _committedBytes ? _reservation.bytes - _committedBytes : 0;
        }

        /// Commits the pages @p newBytes spans past those committed. HostMemory when the system refuses, or
        /// when @p newBytes is past the span, which the store never asks for (it is past the maximum).
        [[nodiscard]] std::expected<void, Refusal> grow(uint64_t newBytes) noexcept override
        {
            if (newBytes > _spanBytes)
                return std::unexpected { Refusal::HostMemory };
            // The span is whole pages, so rounding up within it is a u64.
            auto const committed = *roundedUp(newBytes, _virtualMemory->pageSize());
            if (committed > _committedBytes)
            {
                auto const added = platform::Region { .base = _reservation.base + _committedBytes,
                                                      .bytes = committed - _committedBytes };
                if (auto result = commit(*_virtualMemory, added); !result)
                    return result;
                _committedBytes = committed;
            }
            // Committed pages are zeroed when first committed and never decommitted, and nothing writes past
            // bytes(): the added bytes are zero.
            _size = newBytes;
            return {};
        }

      private:
        platform::IVirtualMemory* _virtualMemory; ///< The address space of the reservation.
        platform::Region _reservation;            ///< The whole reservation, owned.
        uint64_t _spanBytes;                      ///< The most it may commit.
        uint64_t _committedBytes = 0;             ///< The committed pages from its base.
        uint64_t _size = 0;                       ///< Its size, at most _committedBytes.
    };
} // namespace

ReservedMemoryAllocator::ReservedMemoryAllocator(platform::IVirtualMemory& virtualMemory,
                                                 MemoryLayout layout) noexcept:
    _virtualMemory { &virtualMemory }, _layout { layout }
{
    ISOLINE_PRECONDITION(isWellFormed(layout));
}

std::expected<std::unique_ptr<ILinearMemory>, Refusal> ReservedMemoryAllocator::allocate(
    MemoryRequest const& request) noexcept
{
    auto const size = reservationExtentFor(_layout, request.maximumBytes, *_virtualMemory);
    if (!size)
        return std::unexpected { Refusal::HostMemory };
    auto const reservation = _virtualMemory->reserve(size->reservedBytes);
    if (!reservation)
        return std::unexpected { Refusal::HostMemory };
    auto memory = std::unique_ptr<ReservedLinearMemory> {};
    try
    {
        memory = std::make_unique<ReservedLinearMemory>(*_virtualMemory, *reservation, size->spanBytes);
    }
    catch (std::bad_alloc const&)
    {
        // uncovered-begin(bad-alloc)
        std::ignore = _virtualMemory->release(*reservation);
        return std::unexpected { Refusal::HostMemory };
    }
    // uncovered-end(bad-alloc)
    // A refused initial commit destroys the memory, which releases the reservation.
    return memory->grow(request.initialBytes).transform([&] {
        return std::unique_ptr<ILinearMemory> { std::move(memory) };
    });
}

} // namespace isoline::runtime
