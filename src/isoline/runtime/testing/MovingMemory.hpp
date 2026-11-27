// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>

namespace isoline::runtime::testing
{

/// A memory's storage that moves on every grow: the bytes are copied into a new buffer, and the old one is
/// kept, filled with RetiredByte, so that code still reading it reads that, and ASan sees no use after free.
class MovingMemory final: public ILinearMemory
{
  public:
    /// What a retired buffer holds.
    static constexpr auto RetiredByte = std::byte { 0xA5 };

    /// @param size Its size in bytes to begin with, zeroed.
    explicit MovingMemory(uint64_t size): _buffers(1, std::vector<std::byte>(static_cast<std::size_t>(size)))
    {
    }

    [[nodiscard]] std::span<std::byte> bytes() noexcept override { return _buffers.back(); }

    [[nodiscard]] std::span<std::byte const> bytes() const noexcept override { return _buffers.back(); }

    [[nodiscard]] uint64_t guardBytes() const noexcept override { return 0; }

    [[nodiscard]] uint64_t tailBytes() const noexcept override { return 0; }

    /// Moves to a buffer of exactly the new size; it keeps no tail, so it refuses, with Guard, a size that
    /// needs one.
    [[nodiscard]] std::expected<void, Refusal> grow(GrowRequest const& request) noexcept override
    {
        if (request.requiredTailBytes > 0)
            return std::unexpected { Refusal::Guard };
        auto moved = std::vector<std::byte>(static_cast<std::size_t>(request.newBytes));
        std::ranges::copy(_buffers.back(), moved.begin());
        std::ranges::fill(_buffers.back(), RetiredByte);
        _buffers.push_back(std::move(moved));
        return {};
    }

    /// How many times it moved.
    /// @return The grows it took.
    [[nodiscard]] std::size_t moves() const noexcept { return _buffers.size() - 1; }

  private:
    std::vector<std::vector<std::byte>> _buffers; ///< Every buffer it had; the last is the current one.
};

/// Hands out MovingMemory: an embedder whose memories move whenever they grow.
class MovingMemoryAllocator final: public IMemoryAllocator
{
  public:
    [[nodiscard]] std::expected<std::unique_ptr<ILinearMemory>, Refusal> allocate(
        MemoryRequest const& request) noexcept override
    {
        auto memory = std::make_unique<MovingMemory>(request.initialBytes);
        _memories.push_back(memory.get());
        return memory;
    }

    /// The moves of every memory it handed out.
    /// @return Their sum.
    [[nodiscard]] std::size_t moves() const noexcept
    {
        auto total = std::size_t { 0 };
        for (auto const* memory: _memories)
            total += memory->moves();
        return total;
    }

  private:
    std::vector<MovingMemory const*> _memories;
};

} // namespace isoline::runtime::testing
