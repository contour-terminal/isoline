// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/LinearMemory.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <vector>

#include <isoline/runtime/Limits.hpp>

namespace isoline::runtime
{

namespace
{
    /// Resizes @p bytes to @p size, the added bytes zeroed; leaves it unchanged on a refusal.
    /// @return Nothing, or HostMemory when @p size is past what a vector can hold or allocation fails.
    [[nodiscard]] std::expected<void, Refusal> resizeZeroed(std::vector<std::byte>& bytes,
                                                            uint64_t size) noexcept
    {
        if (size > bytes.max_size())
            return std::unexpected { Refusal::HostMemory };
        try
        {
            bytes.resize(static_cast<std::size_t>(size));
        }
        catch (std::bad_alloc const&)
        {
            // uncovered-begin(bad-alloc)
            return std::unexpected { Refusal::HostMemory };
        }
        // uncovered-end(bad-alloc)
        return {};
    }

    /// A linear memory held in a std::vector: moved by every grow, with no guard region, and MaxAccessWidth
    /// bytes past its end, at every size, that no access writes, so they stay zero, and which become the
    /// guest's, zero, when it grows over them.
    class HeapLinearMemory final: public ILinearMemory
    {
      public:
        [[nodiscard]] std::span<std::byte> bytes() noexcept override
        {
            return std::span { _storage }.first(static_cast<std::size_t>(_size));
        }

        [[nodiscard]] std::span<std::byte const> bytes() const noexcept override
        {
            return std::span { _storage }.first(static_cast<std::size_t>(_size));
        }

        [[nodiscard]] uint64_t guardBytes() const noexcept override { return 0; }

        [[nodiscard]] uint64_t tailBytes() const noexcept override { return _storage.size() - _size; }

        /// Keeps Tail bytes past every end, at least what any size needs (requiredTailBytesOf()), so it never
        /// refuses with Guard.
        [[nodiscard]] std::expected<void, Refusal> grow(GrowRequest const& request) noexcept override
        {
            static_assert(Tail >= requiredTailBytesOf(0));
            if (request.newBytes > std::numeric_limits<uint64_t>::max() - Tail)
                return std::unexpected { Refusal::HostMemory };
            return resizeZeroed(_storage, request.newBytes + Tail).transform([&] {
                _size = request.newBytes;
            });
        }

      private:
        /// The bytes kept past the end.
        static constexpr auto Tail = MaxAccessWidth;

        std::vector<std::byte> _storage; ///< The memory's bytes, then the tail.
        uint64_t _size = 0;              ///< The accessible bytes.
    };
} // namespace

std::expected<std::unique_ptr<ILinearMemory>, Refusal> HeapMemoryAllocator::allocate(
    MemoryRequest const& request) noexcept
{
    try
    {
        auto memory = std::make_unique<HeapLinearMemory>();
        return memory->grow(GrowRequest { .newBytes = request.initialBytes }).transform([&] {
            return std::unique_ptr<ILinearMemory> { std::move(memory) };
        });
    }
    catch (std::bad_alloc const&)
    {
        // uncovered-begin(bad-alloc)
        return std::unexpected { Refusal::HostMemory };
    }
    // uncovered-end(bad-alloc)
}

} // namespace isoline::runtime
