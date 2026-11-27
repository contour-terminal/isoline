// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/runtime/HandleTable.hpp>

#include <core/Assert.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <utility>
#include <variant>

#include <isoline/component/canonical/Trap.hpp>

namespace isoline::component::runtime
{

namespace
{

    /// The chunk index @p index lies in, and its offset there: chunk k starts at 64 x (2^k - 1).
    struct ChunkPosition
    {
        std::size_t chunk;  ///< Which chunk.
        std::size_t offset; ///< The slot's offset in it.
    };

    [[nodiscard]] constexpr ChunkPosition chunkPositionOf(uint32_t index) noexcept
    {
        // Chunk k holds the indices whose quotient q by the first chunk's length has 2^k <= q + 1 < 2^(k+1):
        // its first index is the first chunk's length times 2^k - 1, and 2^k is q + 1's highest bit.
        auto const quotient = std::size_t { index / HandleTableFirstChunkLength } + 1;
        auto const highest = std::bit_floor(quotient);
        auto const chunk = static_cast<std::size_t>(std::countr_zero(highest));
        auto const start = std::size_t { HandleTableFirstChunkLength } * (highest - 1);
        return ChunkPosition { .chunk = chunk, .offset = index - start };
    }

    static_assert(chunkPositionOf(0).chunk == 0 && chunkPositionOf(63).offset == 63);
    static_assert(chunkPositionOf(64).chunk == 1 && chunkPositionOf(64).offset == 0);
    static_assert(chunkPositionOf(191).chunk == 1 && chunkPositionOf(191).offset == 127);
    static_assert(chunkPositionOf(192).chunk == 2 && chunkPositionOf(192).offset == 0);
    static_assert(chunkPositionOf(HandleTableMaxLength).chunk == 22);

    /// The Handles trap of a cap of @p limit.
    [[nodiscard]] Trap handlesTrap(uint32_t limit)
    {
        return canonical::trapOf(canonical::TrapCode::Handles, limit);
    }

} // namespace

std::expected<void, Trap> HandleBudget::charge()
{
    if (_used >= _limit)
        return std::unexpected { handlesTrap(_limit) };
    ++_used;
    return {};
}

void HandleBudget::release(uint32_t count) noexcept
{
    Require(count <= _used);
    _used -= count;
}

HandleTable::HandleTable(HandleBudget& budget, uint32_t maxIndex) noexcept:
    _budget { &budget }, _maxIndex { maxIndex }
{
    Require(maxIndex <= HandleTableMaxLength);
}

HandleTable::~HandleTable()
{
    _budget->release(_length - 1);
}

std::expected<uint32_t, Trap> HandleTable::add(HandleEntry entry)
{
    Require(!std::holds_alternative<FreeSlot>(entry));
    if (!_free.empty())
    {
        auto const index = _free.back();
        _free.pop_back();
        slot(index) = entry;
        return index;
    }
    auto const index = _length;
    if (index > _maxIndex)
        return std::unexpected { handlesTrap(_maxIndex) };
    if (auto charged = _budget->charge(); !charged)
        return std::unexpected { std::move(charged.error()) };
    auto const [chunk, offset] = chunkPositionOf(index);
    if (_chunks[chunk].empty())
    {
        try
        {
            _chunks[chunk].resize(std::size_t { HandleTableFirstChunkLength } << chunk);
        }
        catch (...)
        {
            // uncovered-begin(chunk-allocation)
            _budget->release(1);
            throw;
        }
        // uncovered-end(chunk-allocation)
    }
    _chunks[chunk][offset] = entry;
    ++_length;
    return index;
}

std::expected<std::reference_wrapper<HandleEntry>, Trap> HandleTable::get(uint32_t index)
{
    // Index 0 is never a handle, and a table that never grew has no chunk to hold slot 0 at all.
    if (index == 0 || index >= _length || std::holds_alternative<FreeSlot>(slot(index)))
        return std::unexpected { canonical::trapOf(canonical::TrapCode::UnknownHandle, index) };
    return std::ref(slot(index));
}

std::expected<HandleEntry, Trap> HandleTable::remove(uint32_t index)
{
    auto found = get(index);
    if (!found)
        return std::unexpected { std::move(found.error()) };
    _free.push_back(index);
    return std::exchange(found->get(), HandleEntry { FreeSlot {} });
}

HandleEntry& HandleTable::slot(uint32_t index) noexcept
{
    Require(index < _length);
    auto const [chunk, offset] = chunkPositionOf(index);
    return _chunks[chunk][offset];
}

} // namespace isoline::component::runtime
