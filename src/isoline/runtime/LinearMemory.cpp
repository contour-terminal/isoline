// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/LinearMemory.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
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

    /// A linear memory held in a std::vector: moved by every grow, and with no guard region.
    class HeapLinearMemory final: public ILinearMemory
    {
      public:
        [[nodiscard]] std::span<std::byte> bytes() noexcept override { return _bytes; }

        [[nodiscard]] std::span<std::byte const> bytes() const noexcept override { return _bytes; }

        [[nodiscard]] uint64_t guardBytes() const noexcept override { return 0; }

        [[nodiscard]] std::expected<void, Refusal> grow(uint64_t newBytes) noexcept override
        {
            return resizeZeroed(_bytes, newBytes);
        }

      private:
        std::vector<std::byte> _bytes;
    };
} // namespace

std::expected<std::unique_ptr<ILinearMemory>, Refusal> HeapMemoryAllocator::allocate(
    MemoryRequest const& request) noexcept
{
    try
    {
        auto memory = std::make_unique<HeapLinearMemory>();
        return memory->grow(request.initialBytes).transform([&] {
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
