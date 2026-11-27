// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/registerallocation/ParallelMoves.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

namespace isoline::compiler::registerallocation
{

using machine::LocationKind;

namespace
{
    /// No place, no move.
    constexpr auto None = std::numeric_limits<uint32_t>::max();

    /// A key per place, totally ordered: a slot is one place whatever class it holds, a register is one of
    /// its class.
    [[nodiscard]] uint64_t placeOf(Location const& location) noexcept
    {
        auto const registerClass =
            location.kind == LocationKind::Register ? static_cast<uint64_t>(location.registerClass) : 0;
        return (static_cast<uint64_t>(location.kind) << 40U) | (registerClass << 32U) | location.index;
    }

    /// @p place holding a value of @p registerClass.
    [[nodiscard]] Location as(Location place, machine::RegisterClass registerClass) noexcept
    {
        place.registerClass = registerClass;
        return place;
    }

    /// The places the moves name, numbered densely in key order.
    class Places
    {
      public:
        Places(std::span<Move const> moves, Location scratch, std::pmr::memory_resource& memory):
            _keys { &memory }
        {
            _keys.reserve((moves.size() * 2) + 1);
            for (auto const& move: moves)
            {
                _keys.push_back(placeOf(move.from));
                _keys.push_back(placeOf(move.to));
            }
            _keys.push_back(placeOf(scratch));
            std::ranges::sort(_keys);
            auto const repeats = std::ranges::unique(_keys);
            _keys.erase(repeats.begin(), repeats.end());
        }

        [[nodiscard]] uint32_t of(Location const& location) const noexcept
        {
            auto const found = std::ranges::lower_bound(_keys, placeOf(location));
            return static_cast<uint32_t>(found - _keys.begin());
        }

        [[nodiscard]] uint32_t count() const noexcept { return static_cast<uint32_t>(_keys.size()); }

      private:
        std::pmr::vector<uint64_t> _keys;
    };
} // namespace

std::pmr::vector<Move> sequentializeParallelMoves(std::span<Move const> moves,
                                                  Location scratch,
                                                  std::pmr::memory_resource& memory)
{
    Require(scratch.kind != LocationKind::None);
    auto result = std::pmr::vector<Move> { &memory };
    auto pending = std::pmr::vector<Move> { &memory };
    pending.reserve(moves.size());
    for (auto const& move: moves)
    {
        Require(move.from.kind != LocationKind::None && move.to.kind != LocationKind::None);
        Require(!move.from.samePlace(scratch) && !move.to.samePlace(scratch));
        if (!move.from.samePlace(move.to))
            pending.push_back(move);
    }
    if (pending.empty())
        return result;
    result.reserve(pending.size() + (pending.size() / 2) + 1);

    auto const places = Places { pending, scratch, memory };
    auto const scratchPlace = places.of(scratch);
    auto const count = static_cast<uint32_t>(pending.size());
    auto source = std::pmr::vector<uint32_t>(count, None, &memory); ///< Per move, the place it reads now.
    auto readers = std::pmr::vector<uint32_t>(places.count(), 0, &memory);
    auto writer = std::pmr::vector<uint32_t>(places.count(), None, &memory); ///< Per place, its pending move.
    for (auto const index: std::views::iota(uint32_t { 0 }, count))
    {
        source[index] = places.of(pending[index].from);
        ++readers[source[index]];
        auto const destination = places.of(pending[index].to);
        Require(writer[destination] == None); // every destination is a different place
        writer[destination] = index;
    }
    // Per place, the moves reading it, for the rewrite when a cycle is broken.
    auto readerBegin = std::pmr::vector<uint32_t>(places.count() + 1, 0, &memory);
    for (auto const place: source)
        ++readerBegin[place + 1];
    for (auto const place: std::views::iota(uint32_t { 0 }, places.count()))
        readerBegin[place + 1] += readerBegin[place];
    auto readerList = std::pmr::vector<uint32_t>(count, 0, &memory);
    {
        auto fill = std::pmr::vector<uint32_t>(readerBegin.begin(), readerBegin.end() - 1, &memory);
        for (auto const index: std::views::iota(uint32_t { 0 }, count))
            readerList[fill[source[index]]++] = index;
    }

    auto done = std::pmr::vector<uint8_t>(count, 0, &memory);
    auto ready = std::pmr::vector<uint32_t> { &memory };
    for (auto const index: std::views::iota(uint32_t { 0 }, count) | std::views::reverse)
        if (readers[places.of(pending[index].to)] == 0)
            ready.push_back(index);
    auto left = count;
    auto cursor = uint32_t { 0 }; ///< No move below it is pending.
    auto scratchReaders = uint32_t { 0 };
    while (left != 0)
    {
        if (ready.empty())
        {
            // Only cycles are left (and what hangs off them is drained): save the lowest pending move's
            // destination, and let its readers read the copy.
            Require(scratchReaders == 0);
            while (done[cursor] != 0)
                ++cursor;
            auto const& broken = pending[cursor];
            auto const saved = places.of(broken.to);
            result.push_back(Move { .from = broken.to, .to = as(scratch, broken.to.registerClass) });
            for (auto const reader: std::span { readerList }.subspan(
                     readerBegin[saved], readerBegin[saved + 1] - readerBegin[saved]))
                if (done[reader] == 0 && source[reader] == saved)
                {
                    source[reader] = scratchPlace;
                    ++scratchReaders;
                }
            readers[saved] = 0;
            ready.push_back(cursor);
        }
        auto const index = ready.back();
        ready.pop_back();
        auto const& move = pending[index];
        auto from = move.from;
        if (source[index] == scratchPlace)
        {
            from = as(scratch, move.from.registerClass);
            --scratchReaders;
        }
        else
        {
            auto const read = source[index];
            if (--readers[read] == 0 && writer[read] != None && done[writer[read]] == 0)
                ready.push_back(writer[read]);
        }
        result.push_back(Move { .from = from, .to = move.to });
        done[index] = 1;
        --left;
    }
    return result;
}

} // namespace isoline::compiler::registerallocation
